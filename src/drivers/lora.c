/* LoRa can be compiled out with CONFIG_LORAWAN=n. fsm.c and transmit.c call
 * these entry points unconditionally, so stub them rather than dropping the
 * translation unit — keeps the FSM's error paths (tx_pending retry) exercised. */
#include <zephyr/kernel.h>
#include "lora.h"

#if !IS_ENABLED(CONFIG_LORAWAN)

int lora_init(void)         { return -ENOTSUP; }
int lora_session_save(void) { return 0; }

int lora_send(const uint8_t *data, size_t len, bool confirmed)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	ARG_UNUSED(confirmed);
	return -ENOTSUP;
}

#else

#include <zephyr/device.h>
#include <zephyr/lorawan/lorawan.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <LoRaMac.h>
#include <radio.h>

#include "lora.h"
#include "../config/config.h"

LOG_MODULE_REGISTER(lora, LOG_LEVEL_INF);

/* Downlink command ports — one field per port so user can set any subset */
#define DL_PORT_TX_INTERVAL       10
#define DL_PORT_MS_INTERVAL       11
#define DL_PORT_ANOM_WEIGHT_TH    12
#define DL_PORT_ANOM_TEMP_TH      13

/* Link-health recovery tunables (see feedback_lora_link_recovery).
 * Healthy node is invisible to the app layer: unconfirmed uplinks are
 * fire-and-forget, so a moved/dead gateway or a TTN-side session drop would
 * never be noticed. A sparse confirmed "probe" turns each Nth uplink into a
 * link test; losing the ACK drives a DR step-down ladder and finally a rejoin. */
#define PROBE_EVERY_N_UPLINKS  8   /* healthy: 1 confirmed probe per 8 uplinks */
#define LINK_FAIL_REJOIN       4   /* consecutive no-ACKs → invalidate + rejoin */
#define HEALTHY_DR             LORAWAN_DR_3   /* SF9 start; ADR tunes from here */

/* EU868: SFx = 12 - DRx (DR0=SF12 … DR5=SF7). Lower DR = more range/airtime. */
#define DR_TO_SF(dr)  (12 - (dr))

static bool joined;
/* Set when the recovery ladder clears the session for a forced rejoin, so the
 * end-of-cycle save in the FSM does not resurrect the dead session. RAM-only:
 * valid for this wake, which is all that is needed before System OFF. */
static bool session_invalid;

static void downlink_cb(uint8_t port, uint8_t flags, int16_t rssi, int8_t snr,
			uint8_t len, const uint8_t *data)
{
	LOG_INF("Downlink port=%d rssi=%d snr=%d len=%d", port, rssi, snr, len);

	switch (port) {
	case DL_PORT_TX_INTERVAL: {
		if (len != 4) { LOG_WRN("tx_interval: need 4B got %u", len); return; }
		uint32_t val = sys_get_be32(data);
		uint32_t ms;
		config_get_ms_interval(&ms);
		/* TRD 7.1: T_TX >= T_MEAS must hold at all times */
		if (val < ms) {
			LOG_WRN("tx_interval %u < ms_interval %u — rejected", val, ms);
			return;
		}
		config_set_tx_interval(val);
		LOG_INF("tx_interval := %u s", val);
		break;
	}
	case DL_PORT_MS_INTERVAL: {
		if (len != 4) { LOG_WRN("ms_interval: need 4B got %u", len); return; }
		uint32_t val = sys_get_be32(data);
		uint32_t tx;
		config_get_tx_interval(&tx);
		if (val > tx) {
			LOG_WRN("ms_interval %u > tx_interval %u — rejected", val, tx);
			return;
		}
		config_set_ms_interval(val);  /* clamps >= 60 internally */
		LOG_INF("ms_interval := %u s", val);
		break;
	}
	case DL_PORT_ANOM_WEIGHT_TH:
		if (len != 2) { LOG_WRN("anom_weight: need 2B got %u", len); return; }
		config_set_anomaly_weight_threshold(sys_get_be16(data));
		LOG_INF("anom_weight_th := %u", sys_get_be16(data));
		break;
	case DL_PORT_ANOM_TEMP_TH:
		if (len != 2) { LOG_WRN("anom_temp: need 2B got %u", len); return; }
		config_set_anomaly_temp_threshold(sys_get_be16(data));
		LOG_INF("anom_temp_th := %u", sys_get_be16(data));
		break;
	default:
		break;
	}
}

static int session_restore(void)
{
	MibRequestConfirm_t mib_req;

	/* Get pointer to internal NVM struct */
	mib_req.Type = MIB_NVM_CTXS;
	if (LoRaMacMibGetRequestConfirm(&mib_req) != LORAMAC_STATUS_OK) {
		return -EIO;
	}

	LoRaMacNvmData_t *nvm = mib_req.Param.Contexts;
	int ret = config_session_load(nvm, sizeof(*nvm));
	if (ret) {
		return ret;
	}
	/* Data written directly to internal struct — no SET needed */

	/* Verify we have a valid DevAddr */
	mib_req.Type = MIB_DEV_ADDR;
	LoRaMacMibGetRequestConfirm(&mib_req);
	if (mib_req.Param.DevAddr == 0) {
		LOG_WRN("Restored session has no DevAddr");
		return -EINVAL;
	}

	LOG_INF("Session restored, DevAddr: %08x", mib_req.Param.DevAddr);
	return 0;
}

/* Apply DR/ADR/NbTrans policy. MUST run AFTER session_restore(): the restored
 * NVM blob carries the old DR, ADR flag and NbTrans and would otherwise clobber
 * whatever we set in lora_init(). */
static void apply_link_policy(bool fresh_join)
{
	link_state_t st;
	config_get_link_state(&st);

	/* Pin NbTrans=1 so a confirmed no-ACK costs exactly one TX, not an
	 * up-to-8x retransmit burst (battery). Restore clobbers this → set here. */
	lorawan_set_conf_msg_tries(1);

	if (st.forced_dr >= 0) {
		/* Degraded: ADR off (set_datarate needs ADR off) + pinned low DR. */
		lorawan_enable_adr(false);
		int rc = lorawan_set_datarate((enum lorawan_datarate)st.forced_dr);
		LOG_WRN("Link policy: DEGRADED DR%d (SF%d) probe-every-cycle rc=%d",
			st.forced_dr, DR_TO_SF(st.forced_dr), rc);
	} else if (fresh_join) {
		/* Fresh join: known-good default, then let ADR tune down. */
		lorawan_enable_adr(false);
		lorawan_set_datarate(HEALTHY_DR);
		lorawan_enable_adr(true);
		LOG_INF("Link policy: fresh join DR%d + ADR", HEALTHY_DR);
	} else {
		/* Healthy restore: keep the restored (ADR-tuned) DR, but make sure
		 * ADR is on so the node can still escape a pinned-low DR. */
		lorawan_enable_adr(true);
		LOG_INF("Link policy: restored session, ADR on");
	}
}

static int otaa_join(uint8_t join_dr)
{
	uint8_t dev_eui[8], join_eui[8], app_key[16];
	config_get_dev_eui(dev_eui);
	config_get_join_eui(join_eui);
	config_get_app_key(app_key);

	/* HARD requirement: DevNonce MUST be persisted before it goes on air.
	 * If ZMS is down we would burn nonce 0 forever → TTN replay-rejects
	 * every join → retry storm drains the cell. Abort instead; the node
	 * retries next wake (tx_pending path). */
	uint16_t nonce = 0;
	int rc = config_get_dev_nonce(&nonce);
	if (rc) {
		LOG_ERR("DevNonce not persistable (%d) — join aborted", rc);
		memset(app_key, 0, sizeof(app_key));
		return rc;
	}
	/* On-air DevNonce is nonce+1: with CONFIG_LORAWAN_NVM_NONE the Zephyr glue
	 * writes this value into Crypto.DevNonce, then LoRaMacCryptoPrepareJoinRequest
	 * pre-increments it. Matters when clearing used nonces on the network side. */
	LOG_INF("DevNonce: %u (on air %u)", nonce, nonce + 1);
	LOG_INF("DevEUI  %02X%02X%02X%02X%02X%02X%02X%02X  JoinEUI %02X%02X%02X%02X%02X%02X%02X%02X",
		dev_eui[0], dev_eui[1], dev_eui[2], dev_eui[3],
		dev_eui[4], dev_eui[5], dev_eui[6], dev_eui[7],
		join_eui[0], join_eui[1], join_eui[2], join_eui[3],
		join_eui[4], join_eui[5], join_eui[6], join_eui[7]);

	/* Join at the requested DR (ADR off so the value sticks). Far gateway /
	 * first deploy benefits from a lower DR = longer range on JoinRequest. */
	lorawan_enable_adr(false);
	lorawan_set_datarate((enum lorawan_datarate)join_dr);
	LOG_INF("OTAA join at DR%d (SF%d)...", join_dr, DR_TO_SF(join_dr));

	struct lorawan_join_config jcfg = {
		.mode    = LORAWAN_ACT_OTAA,
		.dev_eui = dev_eui,
		.otaa = {
			.join_eui  = join_eui,
			.app_key   = app_key,
			.nwk_key   = app_key,
			.dev_nonce = nonce,
		},
	};

	int ret = lorawan_join(&jcfg);
	memset(app_key, 0, sizeof(app_key));
	if (ret) {
		LOG_ERR("Join failed: %d", ret);
		return ret;
	}

	LOG_INF("Joined TTN");
	return 0;
}

int lora_init(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_ALIAS(lora0));
	if (!device_is_ready(dev)) {
		LOG_ERR("SX1262 not ready");
		return -ENODEV;
	}

	int ret = lorawan_set_region(LORAWAN_REGION_EU868);
	if (ret) {
		LOG_ERR("set_region failed: %d", ret);
		return ret;
	}

	ret = lorawan_start();
	if (ret) {
		LOG_ERR("lorawan_start failed: %d", ret);
		return ret;
	}

	static struct lorawan_downlink_cb dl_cb = {
		.port = LW_RECV_PORT_ANY,
		.cb   = downlink_cb,
	};
	lorawan_register_downlink_callback(&dl_cb);

	/* Try restoring saved session first. DR/ADR/NbTrans policy is applied
	 * AFTER restore so the restored blob does not clobber it. */
	ret = session_restore();
	if (ret == 0) {
		apply_link_policy(false);
		joined = true;
		return 0;
	}

	LOG_INF("No saved session, joining via OTAA");

	link_state_t st;
	config_get_link_state(&st);

	/* Drop join DR one step per prior failed join (clamp DR0) so a far/maint.
	 * gateway is eventually reachable. One attempt per wake keeps it power-safe.
	 * int math + clamp avoids int8 wrap when join_fail is large. */
	int join_dr = (int)HEALTHY_DR - (int)st.join_fail;
	if (join_dr < LORAWAN_DR_0) {
		join_dr = LORAWAN_DR_0;
	}

	ret = otaa_join((uint8_t)join_dr);
	if (ret) {
		if (st.join_fail < 0xFF) {
			st.join_fail++;
		}
		config_set_link_state(&st);
		return ret;
	}

	/* Joined: reset failure counters, start healthy. */
	st.join_fail = 0;
	st.link_fail = 0;
	st.forced_dr = -1;
	config_set_link_state(&st);

	apply_link_policy(true);
	joined = true;
	return 0;
}

int lora_session_save(void)
{
	if (session_invalid) {
		LOG_INF("Session invalidated for rejoin — not saving");
		return 0;
	}

	MibRequestConfirm_t mib_req;

	mib_req.Type = MIB_NVM_CTXS;
	if (LoRaMacMibGetRequestConfirm(&mib_req) != LORAMAC_STATUS_OK) {
		LOG_ERR("Failed to get NVM context");
		return -EIO;
	}

	LoRaMacNvmData_t *nvm = mib_req.Param.Contexts;
	return config_session_save(nvm, sizeof(*nvm));
}

/* Fold a confirmed-probe result into the recovery ladder. ack==true means the
 * network answered (link alive); ack==false means RX2 timed out (no ACK). */
static void link_health_update(link_state_t *st, bool ack)
{
	if (ack) {
		if (st->link_fail || st->forced_dr >= 0) {
			LOG_INF("Link recovered → ADR");
		}
		st->link_fail = 0;
		if (st->forced_dr >= 0) {
			st->forced_dr = -1;
			lorawan_enable_adr(true);
		}
		return;
	}

	st->link_fail++;
	LOG_WRN("Probe no-ACK (%u/%u)", st->link_fail, LINK_FAIL_REJOIN);

	if (st->link_fail >= LINK_FAIL_REJOIN) {
		/* Link is dead — drop the (likely stale) session so the next boot
		 * does a clean OTAA rejoin. Start that rejoin from healthy DR. */
		LOG_ERR("Link dead — clearing session, OTAA rejoin next boot");
		config_session_clear();
		session_invalid = true;
		st->link_fail = 0;
		st->forced_dr = -1;
		return;
	}

	/* Step DR down one (SF up = more range), pin it, probe every cycle. */
	int8_t next = (st->forced_dr < 0) ? (HEALTHY_DR - 1) : (st->forced_dr - 1);
	if (next < LORAWAN_DR_0) {
		next = LORAWAN_DR_0;
	}
	st->forced_dr = next;
	lorawan_enable_adr(false);
	lorawan_set_datarate((enum lorawan_datarate)next);
	LOG_WRN("Degraded: ADR off, DR%d (SF%d)", next, DR_TO_SF(next));
}

int lora_send(const uint8_t *data, size_t len, bool important)
{
	if (!joined) {
		return -ENETDOWN;
	}

	link_state_t st;
	config_get_link_state(&st);
	st.uplink_count++;

	/* Degraded → probe every cycle; healthy → sparse probe. `important`
	 * (e.g. weight anomaly) is always confirmed and doubles as a probe. */
	bool degraded = (st.forced_dr >= 0);
	bool probe = degraded || (st.uplink_count % PROBE_EVERY_N_UPLINKS == 0);
	bool confirmed = important || probe;

	enum lorawan_message_type type = confirmed ? LORAWAN_MSG_CONFIRMED
						   : LORAWAN_MSG_UNCONFIRMED;
	int ret = lorawan_send(1, (uint8_t *)data, len, type);

	if (confirmed) {
		/* ret==0 = ACK received; ret<0 = no ACK (RX2 timeout) = link signal.
		 * The frame went out either way, so this is purely link health. */
		link_health_update(&st, ret == 0);
		LOG_INF("Probe uplink (%zu B) %s", len,
			ret == 0 ? "ACKed" : "no-ACK");
	} else if (ret) {
		LOG_ERR("send failed: %d", ret);
	} else {
		LOG_INF("Uplink sent (%zu bytes, unconfirmed)", len);
	}

	config_set_link_state(&st);
	return ret;
}

#endif /* CONFIG_LORAWAN */
