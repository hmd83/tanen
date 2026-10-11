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

void lora_set_manual(bool on) { ARG_UNUSED(on); }

#else

#include <zephyr/device.h>
#include <zephyr/lorawan/lorawan.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <LoRaMac.h>
#include <LoRaMacCommands.h>
#include <radio.h>

#include "lora.h"
#include "../config/config.h"

LOG_MODULE_REGISTER(lora, LOG_LEVEL_INF);

/* The session blob is sizeof(LoRaMacNvmData_t) as THIS file sees it, so it
 * must see the library's layout (see CMakeLists.txt). Without REGION_EU868 the
 * struct shrinks and the save drops the channel table. */
#if IS_ENABLED(CONFIG_LORAWAN_REGION_EU868)
BUILD_ASSERT(REGION_NVM_MAX_NB_BANDS == 6, "lora.c not built with REGION_EU868");
#endif
#if IS_ENABLED(CONFIG_HAS_SEMTECH_SOFT_SE) && !defined(SOFT_SE)
#error "lora.c not built with SOFT_SE — LoRaMacNvmData_t layout mismatch"
#endif

/* Downlink command ports — one field per port so user can set any subset */
#define DL_PORT_TX_INTERVAL       10
#define DL_PORT_MS_INTERVAL       11
#define DL_PORT_ANOM_WEIGHT_TH    12
#define DL_PORT_ANOM_TEMP_TH      13

/* Link-health recovery tunables (see feedback_lora_link_recovery).
 * Healthy node is invisible to the app layer: unconfirmed uplinks are
 * fire-and-forget, so a moved/dead gateway or a TTN-side session drop would
 * never be noticed. A sparse confirmed "probe" turns each Nth uplink into a
 * link test; losing the ACK drives a DR step-down ladder and, only as a last
 * resort, a rejoin.
 *
 * A missing ACK means the DOWNLINK failed. On a marginal far-gateway link the
 * uplinks can still be landing perfectly, so treating no-ACK as "link dead"
 * destroys a working session (observed 2026-09-24: session 260B9934 walked
 * SF9→SF10→SF11→SF12 in four cycles and was cleared while TTN was receiving
 * the frames). Hence: step DR down, then sit at the floor for a long time. */
#define PROBE_EVERY_N_UPLINKS  4   /* healthy: 1 confirmed probe per 4 uplinks —
                                    * a lost link is noticed within 4 h at a 1 h
                                    * heartbeat, at 6 dl/day (TTN fair use: 10) */
#define PROBE_EVERY_N_DEGRADED 4   /* degraded: NOT every cycle — a shared
                                    * gateway cannot serve a downlink per wake */
#define LINK_FAIL_REJOIN       8   /* consecutive no-ACK probes (step-downs
                                    * included) before a rejoin → 32 uplinks:
                                    * ~32 h at a 1 h heartbeat, ~5 days at 4 h */

/* TTN fair use is 30 s airtime/day. An SF12 frame (uplink or JoinRequest) is
 * ~1.5 s, so an hourly SF12 node is ~36 s/day — over budget. Two brakes:
 *  - data: below DR0_MIN_TX_INTERVAL_S, every other routine uplink at DR0 is
 *    dropped (probes and important frames always go);
 *  - join: once joins have fallen to DR0 and still fail, retry after 1, 2, 4,
 *    then every 8 h instead of every wake (TX-pending retries each wake,
 *    i.e. every 5 min by default = ~430 s/day at SF12). */
BUILD_ASSERT(PROBE_EVERY_N_UPLINKS % 2 == 0 && PROBE_EVERY_N_DEGRADED % 2 == 0,
	     "SF12 fair-use skip drops odd uplinks; probes must land on even ones");
#define DR0_MIN_TX_INTERVAL_S  7200
#define JOIN_BACKOFF_BASE_S    3600
#define JOIN_BACKOFF_MAX_SHIFT 3
#define HEALTHY_DR             LORAWAN_DR_3   /* ADR ceiling. The post-join DR is
                                               * the DR the join actually closed
                                               * at — NOT this. */

/* EU868: SFx = 12 - DRx (DR0=SF12 … DR5=SF7). Lower DR = more range/airtime. */
#define DR_TO_SF(dr)  (12 - (dr))

static bool joined;
/* Activation config this boot's session was set up with (lora_init). */
static lora_mode_t mode;
/* Set when the recovery ladder clears the session for a forced rejoin, so the
 * end-of-cycle save in the FSM does not resurrect the dead session. RAM-only:
 * valid for this wake, which is all that is needed before System OFF. */
static bool session_invalid;
/* Installer is present (BLE LoRa test): skip the fair-use brakes. RAM-only. */
static bool manual;

void lora_set_manual(bool on)
{
	manual = on;
}

/* ABP with a fixed DR: no ADR, no probes, no ladder. Nothing on this path
 * waits for a downlink, which is the point of choosing it. */
static bool fixed_dr(void)
{
	return mode.mode == LORA_MODE_ABP && mode.dr != LORA_DR_ADR;
}

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

/* Uplink MAC answers (LinkADRAns, DevStatusAns, ...) sit in LoRaMac's RAM-only
 * command list (LoRaMacCommands.c CommandsCtx), NOT in the NVM contexts, and
 * go out in the NEXT uplink's FOpts — which is after System OFF. So every
 * answer was lost: the DR change itself was applied and saved, but TTN never
 * saw the LinkADRAns and repeated LinkADRReq after every uplink (observed
 * beelogger2: 466 downlinks for 469 uplinks, stuck at SF12). Persist the list
 * with the session and re-queue it on restore so the next uplink carries it. */
#define MAC_CMDS_BUF  LORA_MAC_COMMAND_MAX_LENGTH

/* Payload size (CID excluded) of end-device answers worth re-queueing; -1 for
 * anything else. LinkCheckReq/DeviceTimeReq are requests whose answer needs
 * MLME state that does not survive the reset either — dropped. */
static int mac_ans_len(uint8_t cid)
{
	switch (cid) {
	case MOTE_MAC_LINK_ADR_ANS:        return 1;
	case MOTE_MAC_DUTY_CYCLE_ANS:      return 0;
	case MOTE_MAC_RX_PARAM_SETUP_ANS:  return 1;
	case MOTE_MAC_DEV_STATUS_ANS:      return 2;
	case MOTE_MAC_NEW_CHANNEL_ANS:     return 1;
	case MOTE_MAC_RX_TIMING_SETUP_ANS: return 0;
	case MOTE_MAC_TX_PARAM_SETUP_ANS:  return 0;
	case MOTE_MAC_DL_CHANNEL_ANS:      return 1;
	default:                           return -1;
	}
}

static int mac_cmds_save(void)
{
	uint8_t buf[MAC_CMDS_BUF];
	size_t len = 0;

	if (LoRaMacCommandsSerializeCmds(sizeof(buf), &len, buf) !=
	    LORAMAC_COMMANDS_SUCCESS) {
		len = 0;
	}
	if (len) {
		LOG_HEXDUMP_INF(buf, len, "Persisting pending MAC answers:");
	}
	return config_mac_cmds_save(buf, len);
}

static void mac_cmds_restore(void)
{
	uint8_t buf[MAC_CMDS_BUF];
	int len = config_mac_cmds_load(buf, sizeof(buf));
	int n = 0;

	for (int i = 0; i < len;) {
		uint8_t cid = buf[i++];
		int plen = mac_ans_len(cid);

		if (plen < 0 || i + plen > len) {
			break;   /* request or unknown CID: size unknown, stop */
		}
		if (LoRaMacCommandsAddCmd(cid, &buf[i], plen) == LORAMAC_COMMANDS_SUCCESS) {
			n++;
		}
		i += plen;
	}
	if (n) {
		LOG_INF("Re-queued %d MAC answer(s) for this uplink", n);
	}
}

/* Debug: which EU868 channels the MAC knows. TTN's LinkADRReq enables ch0-7
 * (mask 0x00FF); if ch3-7 (the join-accept CFList) are undefined the MAC
 * NACKs the mask and rejects the whole command — ADR can never raise the DR.
 * Expect "mask=0x00ff f/100k=8681 8683 8685 8671 8673 8675 8677 8679". */
static void log_channels(const char *tag)
{
	MibRequestConfirm_t mib_req = { .Type = MIB_NVM_CTXS };

	if (LoRaMacMibGetRequestConfirm(&mib_req) != LORAMAC_STATUS_OK) {
		return;
	}
	const RegionNvmDataGroup2_t *g2 = &mib_req.Param.Contexts->RegionGroup2;
	uint16_t f[8];

	for (int i = 0; i < 8; i++) {
		f[i] = (uint16_t)(g2->Channels[i].Frequency / 100000U);  /* 100 kHz */
	}
	LOG_DBG("ch[%s] mask=0x%04x f/100k=%u %u %u %u %u %u %u %u", tag,
		g2->ChannelsMask[0], f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
}

#ifdef SOFT_SE
static bool se_key_is(const SecureElementNvmData_t *se, KeyIdentifier_t id,
		      const uint8_t key[16])
{
	for (int i = 0; i < NUM_OF_KEYS; i++) {
		if (se->KeyList[i].KeyID == id) {
			return !memcmp(se->KeyList[i].KeyValue, key, 16);
		}
	}
	return false;
}
#endif

/* Does the stored session belong to the configured activation? Switching
 * OTAA<->ABP or entering new ABP credentials leaves the old session in flash;
 * restoring it would keep sending under the old identity forever (an ABP
 * session never expires the way a dead OTAA one eventually rejoins). */
static bool session_matches(const LoRaMacNvmData_t *s)
{
	if (mode.mode != LORA_MODE_ABP) {
		return s->MacGroup2.NetworkActivation == ACTIVATION_TYPE_OTAA;
	}
	if (s->MacGroup2.NetworkActivation != ACTIVATION_TYPE_ABP) {
		return false;
	}

	uint8_t addr[4];

	config_get_dev_addr(addr);
	if (s->MacGroup2.DevAddr != sys_get_be32(addr)) {
		return false;
	}
#ifdef SOFT_SE
	uint8_t nwk[16], app[16];
	bool same;

	config_get_nwk_skey(nwk);
	config_get_app_skey(app);
	same = se_key_is(&s->SecureElement, F_NWK_S_INT_KEY, nwk) &&
	       se_key_is(&s->SecureElement, APP_S_KEY, app);
	memset(nwk, 0, sizeof(nwk));
	memset(app, 0, sizeof(app));
	return same;
#else
	return true;
#endif
}

/* Staging copy: the blob is checked before it replaces the MAC's live
 * context, so a rejected one leaves the freshly initialised defaults intact
 * (loading straight into the live struct left half an old session behind on
 * a size mismatch). Static: 1120 B is too much for the callers' stacks. */
static LoRaMacNvmData_t nvm_stage;

static int session_restore(void)
{
	MibRequestConfirm_t mib_req;

	/* Get pointer to internal NVM struct */
	mib_req.Type = MIB_NVM_CTXS;
	if (LoRaMacMibGetRequestConfirm(&mib_req) != LORAMAC_STATUS_OK) {
		return -EIO;
	}

	int ret = config_session_load(&nvm_stage, sizeof(nvm_stage));
	if (ret) {
		return ret;
	}

	if (nvm_stage.MacGroup2.DevAddr == 0) {
		LOG_WRN("Restored session has no DevAddr");
		return -EINVAL;
	}
	if (!session_matches(&nvm_stage)) {
		LOG_WRN("Stored session does not match the %s config, discarded",
			mode.mode == LORA_MODE_ABP ? "ABP" : "OTAA");
		config_session_clear();
		config_mac_cmds_save(NULL, 0);
		return -EINVAL;
	}

	/* Data written directly to internal struct — no SET needed */
	memcpy(mib_req.Param.Contexts, &nvm_stage, sizeof(nvm_stage));

	LOG_INF("Session restored (%s), DevAddr: %08x",
		mode.mode == LORA_MODE_ABP ? "ABP" : "OTAA",
		nvm_stage.MacGroup2.DevAddr);
	return 0;
}

/* DR the MAC is currently on (ADR may have moved it since the join). The
 * step-down ladder must start from here, never from HEALTHY_DR: a join that
 * only closed at DR0 must not be "stepped down" to DR2. */
static int8_t current_dr(void)
{
	MibRequestConfirm_t mib_req = { .Type = MIB_CHANNELS_DATARATE };

	if (LoRaMacMibGetRequestConfirm(&mib_req) != LORAMAC_STATUS_OK) {
		return HEALTHY_DR;
	}
	return mib_req.Param.ChannelsDatarate;
}

/* Apply DR/ADR/NbTrans policy. MUST run AFTER session_restore(): the restored
 * NVM blob carries the old DR, ADR flag and NbTrans and would otherwise clobber
 * whatever we set in lora_init(). join_dr is the DR the OTAA join closed at and
 * is only read when fresh_join; pass -1 otherwise. */
static void apply_link_policy(bool fresh_join, int8_t join_dr)
{
	link_state_t st;
	config_get_link_state(&st);

	/* Pin NbTrans=1 so a confirmed no-ACK costs exactly one TX, not an
	 * up-to-8x retransmit burst (battery). Restore clobbers this → set here. */
	lorawan_set_conf_msg_tries(1);

	if (fixed_dr()) {
		/* Installer's choice, re-applied every wake: the restored blob may
		 * carry a DR from an earlier ADR session. */
		lorawan_enable_adr(false);
		int rc = lorawan_set_datarate((enum lorawan_datarate)mode.dr);
		LOG_INF("Link policy: ABP fixed DR%d (SF%d), ADR off rc=%d",
			mode.dr, DR_TO_SF(mode.dr), rc);
	} else if (st.forced_dr >= 0) {
		/* Degraded: ADR off (set_datarate needs ADR off) + pinned low DR. */
		lorawan_enable_adr(false);
		int rc = lorawan_set_datarate((enum lorawan_datarate)st.forced_dr);
		LOG_WRN("Link policy: DEGRADED DR%d (SF%d) sparse-probe rc=%d",
			st.forced_dr, DR_TO_SF(st.forced_dr), rc);
	} else if (fresh_join) {
		/* Start where the join actually closed, then let ADR tune UP from
		 * there (ABP has no join: join_dr is -1, it starts at HEALTHY_DR).
		 * Jumping to HEALTHY_DR here threw away the one piece of
		 * measured link knowledge we have: a join that needed SF12 will not
		 * carry a data uplink at SF9, so the first frame was always lost and
		 * the recovery ladder fired on a healthy session. */
		int8_t start = (join_dr >= LORAWAN_DR_0 && join_dr <= HEALTHY_DR)
			     ? join_dr : HEALTHY_DR;

		lorawan_enable_adr(false);
		lorawan_set_datarate((enum lorawan_datarate)start);
		lorawan_enable_adr(true);
		LOG_INF("Link policy: fresh join DR%d (SF%d) + ADR",
			start, DR_TO_SF(start));
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

/* ABP "join": no radio traffic, the MAC is simply handed the session. The
 * MAC keeps its band defaults (3 channels, RX1 1 s, RX2 869.525 MHz DR0),
 * which is what TTN assumes for an ABP device it was told nothing about. TTN
 * moves them to its own values (RXParamSetupReq, NewChannelReq, ...) whenever
 * a downlink does get through; until then both sides agree. */
static int abp_activate(void)
{
	uint8_t dev_eui[8], join_eui[8], addr[4], nwk_skey[16], app_skey[16];

	if (!config_abp_ready()) {
		LOG_ERR("ABP: DevAddr/NwkSKey/AppSKey not set");
		return -EINVAL;
	}
	config_get_dev_eui(dev_eui);
	config_get_join_eui(join_eui);
	config_get_dev_addr(addr);
	config_get_nwk_skey(nwk_skey);
	config_get_app_skey(app_skey);

	uint32_t dev_addr = sys_get_be32(addr);
	struct lorawan_join_config jcfg = {
		.mode    = LORAWAN_ACT_ABP,
		.dev_eui = dev_eui,
		.abp = {
			.dev_addr = dev_addr,
			.app_skey = app_skey,
			.nwk_skey = nwk_skey,
			.app_eui  = join_eui,
		},
	};

	int ret = lorawan_join(&jcfg);
	memset(nwk_skey, 0, sizeof(nwk_skey));
	memset(app_skey, 0, sizeof(app_skey));
	if (ret) {
		LOG_ERR("ABP activation failed: %d", ret);
		return ret;
	}

	/* The MAC zeroed FCntUp at start. Carry on above the last frame sent
	 * with this DevAddr, or TTN drops every frame until the count passes it. */
	uint32_t fcnt = 0;
	MibRequestConfirm_t mib_req = { .Type = MIB_NVM_CTXS };

	config_get_abp_fcnt(dev_addr, &fcnt);
	if (LoRaMacMibGetRequestConfirm(&mib_req) != LORAMAC_STATUS_OK) {
		return -EIO;
	}
	mib_req.Param.Contexts->Crypto.FCntList.FCntUp = fcnt;

	LOG_INF("ABP activated, DevAddr %08x, next FCnt %u", dev_addr, fcnt + 1);
	return 0;
}

int lora_init(void)
{
	/* SETUP's BLE LoRa test and the FSM's TRANSMISSION can both run in one
	 * boot. A second pass must not restart the MAC under the live session
	 * (and restore the older flash copy over it), nor re-append the static
	 * downlink node: appending the list's own tail makes dl_callbacks a
	 * self-loop, and the next downlink spins until the watchdog resets
	 * (observed: reset right after the first FSM uplink following SETUP). */
	static bool started;
	int ret;

	if (joined) {
		return 0;
	}

	if (!started) {
		const struct device *dev = DEVICE_DT_GET(DT_ALIAS(lora0));
		if (!device_is_ready(dev)) {
			LOG_ERR("SX1262 not ready");
			return -ENODEV;
		}

		ret = lorawan_set_region(LORAWAN_REGION_EU868);
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
		started = true;
	}

	config_get_lora_mode(&mode);

	/* Try restoring saved session first. DR/ADR/NbTrans policy is applied
	 * AFTER restore so the restored blob does not clobber it. */
	ret = session_restore();
	if (ret == 0) {
		apply_link_policy(false, -1);
		log_channels("restored");
		mac_cmds_restore();
		joined = true;
		return 0;
	}

	link_state_t st;
	config_get_link_state(&st);

	if (mode.mode == LORA_MODE_ABP) {
		LOG_INF("No saved session, activating via ABP");
		ret = abp_activate();
		if (ret) {
			return ret;
		}
		/* A fresh session starts healthy, as after a join. */
		st.join_fail = 0;
		st.join_wait = 0;
		st.link_fail = 0;
		st.forced_dr = -1;
		config_set_link_state(&st);
		config_mac_cmds_save(NULL, 0);

		apply_link_policy(true, -1);
		log_channels("abp");
		joined = true;
		return 0;
	}

	LOG_INF("No saved session, joining via OTAA");

	/* Drop join DR one step per prior failed join (clamp DR0) so a far/maint.
	 * gateway is eventually reachable. One attempt per wake keeps it power-safe.
	 * int math + clamp avoids int8 wrap when join_fail is large. */
	int join_dr = (int)HEALTHY_DR - (int)st.join_fail;
	if (join_dr < LORAWAN_DR_0) {
		join_dr = LORAWAN_DR_0;
	}

	/* SF12 join backoff: count wakes down instead of transmitting. No
	 * DevNonce is consumed while waiting. */
	if (st.join_wait && !manual) {
		st.join_wait--;
		config_set_link_state(&st);
		LOG_INF("Join backoff: next SF12 try in %u wake(s)", st.join_wait);
		return -EAGAIN;
	}

	ret = otaa_join((uint8_t)join_dr);
	if (ret) {
		if (st.join_fail < 0xFF) {
			st.join_fail++;
		}
		if (join_dr == LORAWAN_DR_0) {
			/* k-th failure at DR0 → wait BASE << min(k-1, MAX_SHIFT) */
			int k = (int)st.join_fail - (HEALTHY_DR - LORAWAN_DR_0);
			uint32_t wait_s = JOIN_BACKOFF_BASE_S <<
					  MIN(MAX(k - 1, 0), JOIN_BACKOFF_MAX_SHIFT);
			uint32_t ms;

			config_get_ms_interval(&ms);
			st.join_wait = MIN(wait_s / MAX(ms, 1U), UINT16_MAX);
			LOG_WRN("SF12 join failed — backing off %u s (%u wakes)",
				wait_s, st.join_wait);
		}
		config_set_link_state(&st);
		return ret;
	}

	/* Joined: reset failure counters, start healthy. */
	st.join_fail = 0;
	st.join_wait = 0;
	st.link_fail = 0;
	st.forced_dr = -1;
	config_set_link_state(&st);
	config_mac_cmds_save(NULL, 0);   /* answers to the old session are void */

	apply_link_policy(true, (int8_t)join_dr);
	log_channels("joined");
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

	log_channels("save");
	LoRaMacNvmData_t *nvm = mib_req.Param.Contexts;
	int ret = config_session_save(nvm, sizeof(*nvm));

	if (nvm->MacGroup2.NetworkActivation == ACTIVATION_TYPE_ABP) {
		config_set_abp_fcnt(nvm->MacGroup2.DevAddr,
				    nvm->Crypto.FCntList.FCntUp);
	}
	mac_cmds_save();
	return ret;
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

	if (st->link_fail < 0xFF) {
		st->link_fail++;
	}

	/* Step DR down one (SF up = more range) from wherever we actually are. */
	int8_t base = (st->forced_dr >= 0) ? st->forced_dr : current_dr();
	int8_t next = base - 1;

	if (next < LORAWAN_DR_0) {
		next = LORAWAN_DR_0;
	}

	if (next != st->forced_dr) {
		st->forced_dr = next;
		lorawan_enable_adr(false);
		lorawan_set_datarate((enum lorawan_datarate)next);
		LOG_WRN("Degraded: ADR off, DR%d (SF%d) (%u no-ACK)",
			next, DR_TO_SF(next), st->link_fail);
		return;   /* give the new DR a chance before escalating */
	}

	/* Already at the floor. The uplinks may still be reaching the network —
	 * only the ACK is missing — so do NOT burn the session here. Rejoin is a
	 * last resort, and LINK_FAIL_REJOIN makes it days away, not minutes. */
	LOG_WRN("DR floor: probe no-ACK (%u/%u)", st->link_fail, LINK_FAIL_REJOIN);

	if (st->link_fail >= LINK_FAIL_REJOIN && mode.mode == LORA_MODE_ABP) {
		/* No join to fall back on, and none needed: the keys are fixed,
		 * so the session cannot have gone stale on the network side.
		 * Keep probing at the floor; the first ACK restores ADR. */
		LOG_WRN("ABP: no downlink for %u probes, staying at DR%d",
			st->link_fail, st->forced_dr);
	} else if (st->link_fail >= LINK_FAIL_REJOIN) {
		LOG_ERR("No downlink for %u probes at DR%d — OTAA rejoin next boot",
			st->link_fail, st->forced_dr);
		config_session_clear();
		config_mac_cmds_save(NULL, 0);
		session_invalid = true;
		st->link_fail = 0;
		st->forced_dr = -1;
	}
}

int lora_send(const uint8_t *data, size_t len, bool important)
{
	if (!joined) {
		return -ENETDOWN;
	}

	link_state_t st;
	config_get_link_state(&st);
	st.uplink_count++;

	/* Sparse probes either way — degraded just probes a bit more often.
	 * Probing every cycle asked a shared community gateway for a downlink
	 * every 5 min, which it cannot schedule, which guaranteed the no-ACK that
	 * kept the node degraded. `important` (e.g. weight anomaly) is always
	 * confirmed and doubles as a probe. */
	bool degraded = (st.forced_dr >= 0);
	uint16_t cadence = degraded ? PROBE_EVERY_N_DEGRADED : PROBE_EVERY_N_UPLINKS;
	bool probe = (st.uplink_count % cadence == 0);
	/* Fixed DR: nothing would act on an ACK, so never ask for one. Each
	 * confirmed frame costs a downlink from TTN's 10/day fair use. */
	bool confirmed = !fixed_dr() && (important || probe);

	/* SF12 fair use: drop every other routine uplink. Probes fall on
	 * multiples of 4 — always even — so the odd ones skipped are never
	 * probes and the ladder keeps its cadence. `important` is checked on its
	 * own: with a fixed ABP DR it goes unconfirmed but must still go. */
	uint32_t tx_int;

	config_get_tx_interval(&tx_int);
	if (!confirmed && !important && !manual && (st.uplink_count & 1) &&
	    tx_int < DR0_MIN_TX_INTERVAL_S && current_dr() == LORAWAN_DR_0) {
		config_set_link_state(&st);
		LOG_INF("SF12 fair use: routine uplink skipped");
		return LORA_TX_SKIPPED;
	}

	enum lorawan_message_type type = confirmed ? LORAWAN_MSG_CONFIRMED
						   : LORAWAN_MSG_UNCONFIRMED;
	int ret = lorawan_send(1, (uint8_t *)data, len, type);

	int tx_ret = ret;

	if (confirmed) {
		/* ret==0 = ACK received; -ETIMEDOUT = the frame went on air but no
		 * ACK arrived in RX1/RX2. Anything else failed before the air and is
		 * a real TX failure worth retrying. */
		link_health_update(&st, ret == 0);

		if (ret == -ETIMEDOUT) {
			/* Report success: the uplink IS delivered as far as we can
			 * tell, and only link health depends on the ACK. Returning
			 * the error made the FSM set tx_pending and retry on the very
			 * next wake, collapsing the 4 h heartbeat into a per-wake
			 * transmit (474 s/day airtime vs TTN's 30 s fair use). */
			tx_ret = 0;
		}
		LOG_INF("Probe uplink (%zu B) %s", len,
			ret == 0 ? "ACKed" : "no-ACK");
	} else if (ret) {
		LOG_ERR("send failed: %d", ret);
	} else {
		LOG_INF("Uplink sent (%zu bytes, unconfirmed)", len);
	}

	config_set_link_state(&st);
	return tx_ret;
}

#endif /* CONFIG_LORAWAN */
