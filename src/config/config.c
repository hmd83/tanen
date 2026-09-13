#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/zms.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdlib.h>

#include "config.h"

LOG_MODULE_REGISTER(config, LOG_LEVEL_DBG);

/* ZMS IDs */
#define ZMS_DEV_NONCE_ID              1  /* monotonic counter, LoRaWAN replay-prot */
#define ZMS_SESSION_ID                2
#define ZMS_DEV_EUI_ID                3
#define ZMS_JOIN_EUI_ID               4
#define ZMS_APP_KEY_ID                5
#define ZMS_ZERO_OFFSET_ID            6
#define ZMS_SCALE_FACTOR_ID           7
#define ZMS_TX_INTERVAL_ID            8
#define ZMS_MS_INTERVAL_ID            9
#define ZMS_ANOMALY_WEIGHT_THRESH_ID  10
#define ZMS_ANOMALY_TEMP_THRESH_ID    11
#define ZMS_LAST_TX_WEIGHT_ID         12
#define ZMS_LAST_TX_TEMP_ID           13
#define ZMS_MEASUREMENT_COUNT_ID      14
#define ZMS_OCAL1_ID                  15
#define ZMS_LINK_STATE_ID             16  /* LoRaWAN link-health recovery state */
#define ZMS_TX_PENDING_ID             17  /* failed-uplink retry flag */
#define ZMS_TEMPCOMP_STATE_ID         18  /* load-cell thermal-lag filter state */
#define ZMS_TARE_TEMP_ID              19  /* temperature at tare = correction T_ref */
#define ZMS_EXT_CONFIG_ID             20  /* Extended Mode BLE sensor slots */

/* ext_config_t is the ZMS record and the GATT value — no padding allowed */
BUILD_ASSERT(sizeof(ext_config_t) == 2 + EXT_SLOTS * 8);

static struct zms_fs zms;
static bool zms_ready;

/* Cached state */
static uint8_t dev_eui[8];
static uint8_t join_eui[8];
static uint8_t app_key[16];
static int32_t zero_offset;
static int32_t scale_factor;
static uint32_t tx_interval;
static uint32_t ms_interval;
static uint16_t anomaly_weight_threshold;
static uint16_t anomaly_temp_threshold;
static uint32_t last_tx_weight;
static int16_t last_tx_temp;
static uint32_t measurement_count;
static int32_t tare_temp_mdeg;
static ext_config_t ext_cfg;

static int hexstr_to_bytes(const char *hex, uint8_t *out, size_t len)
{
	if (strlen(hex) != len * 2) {
		LOG_ERR("hex cred: bad length (want %zu chars)", len * 2);
		memset(out, 0, len);
		return -EINVAL;
	}
	for (size_t i = 0; i < len; i++) {
		char tmp[3] = {hex[i * 2], hex[i * 2 + 1], 0};
		char *end = NULL;
		unsigned long v = strtoul(tmp, &end, 16);
		if (end != &tmp[2]) {
			LOG_ERR("hex cred: invalid char at %zu", i * 2);
			memset(out, 0, len);
			return -EINVAL;
		}
		out[i] = (uint8_t)v;
	}
	return 0;
}

/* Derive a per-device DevEUI from the SoC's factory-unique hardware ID.
 * Used when neither ZMS nor a fixed CONFIG_TANENBASE_DEV_EUI provides one, so
 * every unprovisioned unit gets a distinct EUI instead of a shared constant.
 * The U/L bit is set (and I/G cleared) to mark it locally administered, so a
 * derived EUI can never collide with a real OUI-assigned DevEUI block. */
static void derive_dev_eui_from_hwid(uint8_t out[8])
{
	uint8_t hwid[16];
	ssize_t n = hwinfo_get_device_id(hwid, sizeof(hwid));

	if (n <= 0) {
		LOG_ERR("hwinfo_get_device_id: %zd", n);
		memset(out, 0, 8);
	} else {
		size_t take = (n < 8) ? (size_t)n : 8;
		memset(out, 0, 8);
		memcpy(out, hwid, take);
	}
	out[0] = (out[0] & 0xFE) | 0x02;  /* locally administered, unicast */
	LOG_INF("DevEUI derived from HW ID: %02X%02X%02X%02X%02X%02X%02X%02X",
		out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
}

/* Try loading from ZMS; return true if found */
static bool zms_load(uint32_t id, void *data, size_t len)
{
	if (!zms_ready) {
		return false;
	}
	ssize_t rd = zms_read(&zms, id, data, len);
	return (rd == (ssize_t)len);
}

static int zms_store(uint32_t id, const void *data, size_t len)
{
	if (!zms_ready) {
		return -ENODEV;
	}
	ssize_t wr = zms_write(&zms, id, data, len);
	if (wr < 0) {
		LOG_ERR("ZMS write id=%u: %zd", id, wr);
		return (int)wr;
	}
	return 0;
}

int config_init(void)
{
	/* Mount ZMS on storage partition */
	const struct flash_area *fa;
	struct flash_sector hw_sector;
	uint32_t sector_cnt = 1;

	int rc = flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa);
	if (rc) {
		LOG_ERR("flash_area_open: %d", rc);
		return rc;
	}

	rc = flash_area_get_sectors(FIXED_PARTITION_ID(storage_partition),
				    &sector_cnt, &hw_sector);
	if (rc && rc != -ENOMEM) {
		LOG_ERR("flash_area_get_sectors: %d", rc);
		return rc;
	}

	zms.flash_device = fa->fa_dev;
	zms.offset = fa->fa_off;
	zms.sector_size = hw_sector.fs_size;
	zms.sector_count = fa->fa_size / hw_sector.fs_size;

	LOG_DBG("ZMS: off=0x%lx sec_size=0x%x cnt=%u",
		(long)zms.offset, zms.sector_size, zms.sector_count);

	rc = zms_mount(&zms);
	if (rc) {
		LOG_ERR("zms_mount: %d", rc);
		return rc;
	}
	zms_ready = true;

	/* Load credentials: ZMS first, fallback to Kconfig */
	if (!zms_load(ZMS_DEV_EUI_ID, dev_eui, sizeof(dev_eui))) {
		/* No stored EUI: use a fixed CONFIG override if given (16 hex
		 * chars), else derive a unique one from the SoC hardware ID. */
		if (strlen(CONFIG_TANENBASE_DEV_EUI) == 16) {
			hexstr_to_bytes(CONFIG_TANENBASE_DEV_EUI, dev_eui, 8);
		} else {
			derive_dev_eui_from_hwid(dev_eui);
		}
	}
	if (!zms_load(ZMS_JOIN_EUI_ID, join_eui, sizeof(join_eui))) {
		hexstr_to_bytes(CONFIG_TANENBASE_JOIN_EUI, join_eui, 8);
	}
	if (!zms_load(ZMS_APP_KEY_ID, app_key, sizeof(app_key))) {
		hexstr_to_bytes(CONFIG_TANENBASE_APP_KEY, app_key, 16);
	}

	/* Load calibration with defaults */
	if (!zms_load(ZMS_ZERO_OFFSET_ID, &zero_offset, sizeof(zero_offset))) {
		zero_offset = 0;
	}
	if (!zms_load(ZMS_SCALE_FACTOR_ID, &scale_factor, sizeof(scale_factor))) {
		scale_factor = 1000;
	}
	/* Never tared: fall back to the temperature the coefficient was fitted
	 * at, so the correction is at least anchored somewhere sane. */
	if (!zms_load(ZMS_TARE_TEMP_ID, &tare_temp_mdeg, sizeof(tare_temp_mdeg))) {
		tare_temp_mdeg = CONFIG_TANENBASE_TEMPCOMP_T_REF_MDEG;
	}

	/* Load timing with Kconfig defaults */
	if (!zms_load(ZMS_TX_INTERVAL_ID, &tx_interval, sizeof(tx_interval))) {
		tx_interval = CONFIG_TANENBASE_DEFAULT_TX_INTERVAL;
	}
	if (!zms_load(ZMS_MS_INTERVAL_ID, &ms_interval, sizeof(ms_interval))) {
		ms_interval = CONFIG_TANENBASE_DEFAULT_MS_INTERVAL;
	}

	/* Load anomaly thresholds */
	if (!zms_load(ZMS_ANOMALY_WEIGHT_THRESH_ID, &anomaly_weight_threshold,
		      sizeof(anomaly_weight_threshold))) {
		anomaly_weight_threshold = CONFIG_TANENBASE_DEFAULT_WEIGHT_THRESHOLD;
	}
	if (!zms_load(ZMS_ANOMALY_TEMP_THRESH_ID, &anomaly_temp_threshold,
		      sizeof(anomaly_temp_threshold))) {
		anomaly_temp_threshold = CONFIG_TANENBASE_DEFAULT_TEMP_THRESHOLD;
	}

	/* Load delta tracking state */
	if (!zms_load(ZMS_LAST_TX_WEIGHT_ID, &last_tx_weight, sizeof(last_tx_weight))) {
		last_tx_weight = 0;
	}
	if (!zms_load(ZMS_LAST_TX_TEMP_ID, &last_tx_temp, sizeof(last_tx_temp))) {
		last_tx_temp = 0;
	}
	if (!zms_load(ZMS_MEASUREMENT_COUNT_ID, &measurement_count,
		      sizeof(measurement_count))) {
		measurement_count = 0;
	}

	/* Extended Mode: absent, wrong size or rule-breaking record = disabled */
	if (!zms_load(ZMS_EXT_CONFIG_ID, &ext_cfg, sizeof(ext_cfg)) ||
	    config_ext_validate(&ext_cfg)) {
		memset(&ext_cfg, 0, sizeof(ext_cfg));
		ext_cfg.version = EXT_CONFIG_VERSION;
	}

	LOG_INF("Config loaded (tx_int=%us ms_int=%us wt=%u tt=%u cnt=%u)",
		tx_interval, ms_interval, anomaly_weight_threshold,
		anomaly_temp_threshold, measurement_count);
	return 0;
}

/* LoRaWAN credentials */
int config_get_dev_eui(uint8_t buf[8])   { memcpy(buf, dev_eui, 8);  return 0; }
int config_get_join_eui(uint8_t buf[8])  { memcpy(buf, join_eui, 8); return 0; }
int config_get_app_key(uint8_t buf[16])  { memcpy(buf, app_key, 16); return 0; }

int config_set_dev_eui(const uint8_t buf[8])
{
	memcpy(dev_eui, buf, 8);
	return zms_store(ZMS_DEV_EUI_ID, dev_eui, 8);
}

int config_set_join_eui(const uint8_t buf[8])
{
	memcpy(join_eui, buf, 8);
	return zms_store(ZMS_JOIN_EUI_ID, join_eui, 8);
}

int config_set_app_key(const uint8_t buf[16])
{
	memcpy(app_key, buf, 16);
	return zms_store(ZMS_APP_KEY_ID, app_key, 16);
}

/* DevNonce: LoRaWAN 1.0.4/1.1 requires monotonic increasing per DevEUI.
 * TTN rejects reused nonces (replay protection) → join fails silently → battery
 * dies on retry storm. Persist counter in ZMS, increment on each call. */
int config_get_dev_nonce(uint16_t *nonce)
{
	uint16_t v = 0;
	(void)zms_load(ZMS_DEV_NONCE_ID, &v, sizeof(v));  /* 0 if first boot */
	v++;
	int rc = zms_store(ZMS_DEV_NONCE_ID, &v, sizeof(v));
	if (rc) {
		LOG_ERR("DevNonce persist failed: %d", rc);
		return rc;
	}
	*nonce = v;
	return 0;
}

/* Session persistence */
int config_session_save(const void *data, size_t len)
{
	if (!zms_ready) {
		return -ENODEV;
	}
	ssize_t wr = zms_write(&zms, ZMS_SESSION_ID, data, len);
	if (wr < 0) {
		LOG_ERR("session save: %zd", wr);
		return (int)wr;
	}
	LOG_INF("Session saved (%zu bytes)", len);
	return 0;
}

int config_session_load(void *data, size_t len)
{
	if (!zms_ready) {
		return -ENODEV;
	}
	ssize_t rd = zms_read(&zms, ZMS_SESSION_ID, data, len);
	if (rd < 0) {
		return (int)rd;
	}
	if ((size_t)rd != len) {
		LOG_WRN("Session size mismatch: got %zd, expected %zu", rd, len);
		return -EINVAL;
	}
	LOG_INF("Session restored (%zu bytes)", len);
	return 0;
}

int config_session_clear(void)
{
	if (!zms_ready) {
		return -ENODEV;
	}
	return zms_delete(&zms, ZMS_SESSION_ID);
}

/* Link-health state. Defaults to healthy (forced_dr=-1) when never stored. */
int config_get_link_state(link_state_t *st)
{
	if (!zms_load(ZMS_LINK_STATE_ID, st, sizeof(*st))) {
		st->uplink_count = 0;
		st->link_fail = 0;
		st->join_fail = 0;
		st->forced_dr = -1;
	}
	return 0;
}

int config_set_link_state(const link_state_t *st)
{
	return zms_store(ZMS_LINK_STATE_ID, st, sizeof(*st));
}

/* Calibration */
int config_get_zero_offset(int32_t *val)  { *val = zero_offset; return 0; }
int config_set_zero_offset(int32_t val)
{
	zero_offset = val;
	return zms_store(ZMS_ZERO_OFFSET_ID, &zero_offset, sizeof(zero_offset));
}

int config_get_scale_factor(int32_t *val) { *val = scale_factor; return 0; }
int config_set_scale_factor(int32_t val)
{
	scale_factor = val;
	return zms_store(ZMS_SCALE_FACTOR_ID, &scale_factor, sizeof(scale_factor));
}

/* Timing */
int config_get_tx_interval(uint32_t *val) { *val = tx_interval; return 0; }
int config_set_tx_interval(uint32_t val)
{
	tx_interval = val;
	return zms_store(ZMS_TX_INTERVAL_ID, &tx_interval, sizeof(tx_interval));
}

int config_get_ms_interval(uint32_t *val) { *val = ms_interval; return 0; }
int config_set_ms_interval(uint32_t val)
{
	if (val < 60) {
		val = 60;
	}
	ms_interval = val;
	return zms_store(ZMS_MS_INTERVAL_ID, &ms_interval, sizeof(ms_interval));
}

/* Anomaly thresholds */
int config_get_anomaly_weight_threshold(uint16_t *val) { *val = anomaly_weight_threshold; return 0; }
int config_set_anomaly_weight_threshold(uint16_t val)
{
	anomaly_weight_threshold = val;
	return zms_store(ZMS_ANOMALY_WEIGHT_THRESH_ID, &anomaly_weight_threshold,
			 sizeof(anomaly_weight_threshold));
}

int config_get_anomaly_temp_threshold(uint16_t *val) { *val = anomaly_temp_threshold; return 0; }
int config_set_anomaly_temp_threshold(uint16_t val)
{
	anomaly_temp_threshold = val;
	return zms_store(ZMS_ANOMALY_TEMP_THRESH_ID, &anomaly_temp_threshold,
			 sizeof(anomaly_temp_threshold));
}

/* Delta tracking */
/* Widened uint16→uint32 (999 kg payload support). A pre-upgrade 2-byte ZMS
 * entry won't match this 4-byte read, so zms_load() falls back to 0 once —
 * one harmless reset of the delta baseline on the first post-flash cycle. */
int config_get_last_tx_weight(uint32_t *val) { *val = last_tx_weight; return 0; }
int config_set_last_tx_weight(uint32_t val)
{
	last_tx_weight = val;
	return zms_store(ZMS_LAST_TX_WEIGHT_ID, &last_tx_weight, sizeof(last_tx_weight));
}

int config_get_last_tx_temp(int16_t *val) { *val = last_tx_temp; return 0; }
int config_set_last_tx_temp(int16_t val)
{
	last_tx_temp = val;
	return zms_store(ZMS_LAST_TX_TEMP_ID, &last_tx_temp, sizeof(last_tx_temp));
}

/* Measurement counter */
int config_get_measurement_count(uint32_t *val) { *val = measurement_count; return 0; }
int config_set_measurement_count(uint32_t val)
{
	measurement_count = val;
	return zms_store(ZMS_MEASUREMENT_COUNT_ID, &measurement_count,
			 sizeof(measurement_count));
}

/* TX-pending retry flag — not cached (read once per wake) */
int config_get_tx_pending(uint8_t *val)
{
	if (!zms_load(ZMS_TX_PENDING_ID, val, sizeof(*val))) {
		*val = 0;
	}
	return 0;
}

int config_set_tx_pending(uint8_t val)
{
	/* Skip redundant writes — this is touched every TX cycle */
	uint8_t cur = 0;
	(void)zms_load(ZMS_TX_PENDING_ID, &cur, sizeof(cur));
	if (cur == val) {
		return 0;
	}
	return zms_store(ZMS_TX_PENDING_ID, &val, sizeof(val));
}

/* NAU7802 OCAL1 — not cached (rarely read, only on cold init) */
int config_get_ocal1(int32_t *val)
{
	return zms_load(ZMS_OCAL1_ID, val, sizeof(*val)) ? 0 : -ENOENT;
}

int config_set_ocal1(int32_t val)
{
	return zms_store(ZMS_OCAL1_ID, &val, sizeof(val));
}

/* Tempcomp filter state — not cached (read once per wake, written once per
 * cycle, same wear profile as measurement_count). Defaults to unprimed so the
 * first cycle after a flash seeds t_eff from the live reading. */
int config_get_tempcomp_state(tempcomp_state_t *st)
{
	if (!zms_load(ZMS_TEMPCOMP_STATE_ID, st, sizeof(*st))) {
		st->t_eff_mdeg = 0;
		st->primed = 0;
	}
	return 0;
}

int config_set_tempcomp_state(const tempcomp_state_t *st)
{
	return zms_store(ZMS_TEMPCOMP_STATE_ID, st, sizeof(*st));
}

/* Tare temperature */
int config_get_tare_temp(int32_t *val) { *val = tare_temp_mdeg; return 0; }
int config_set_tare_temp(int32_t val)
{
	tare_temp_mdeg = val;
	return zms_store(ZMS_TARE_TEMP_ID, &tare_temp_mdeg, sizeof(tare_temp_mdeg));
}

/* Extended Mode */
int config_ext_validate(const ext_config_t *cfg)
{
	static const uint8_t zero_mac[6];
	int n_in = 0;
	int n_out = 0;

	if (cfg->version != EXT_CONFIG_VERSION || cfg->enabled > 1) {
		return -EINVAL;
	}
	for (int i = 0; i < EXT_SLOTS; i++) {
		const ext_slot_t *s = &cfg->slot[i];

		if (s->en > 1 || s->role > EXT_ROLE_OUT) {
			return -EINVAL;
		}
		if (!s->en) {
			continue;
		}
		if (!memcmp(s->mac, zero_mac, sizeof(zero_mac))) {
			return -EINVAL;
		}
		for (int j = 0; j < i; j++) {
			if (cfg->slot[j].en && !memcmp(cfg->slot[j].mac, s->mac, sizeof(s->mac))) {
				return -EINVAL;
			}
		}
		if (s->role == EXT_ROLE_OUT) {
			n_out++;
		} else {
			n_in++;
		}
	}
	return (n_in > EXT_MAX_IN || n_out > EXT_MAX_OUT) ? -EINVAL : 0;
}

int config_get_ext(ext_config_t *cfg) { *cfg = ext_cfg; return 0; }
int config_set_ext(const ext_config_t *cfg)
{
	int rc = config_ext_validate(cfg);

	if (rc) {
		return rc;
	}
	ext_cfg = *cfg;
	return zms_store(ZMS_EXT_CONFIG_ID, &ext_cfg, sizeof(ext_cfg));
}

bool config_ext_active(void)
{
	if (!ext_cfg.enabled) {
		return false;
	}
	for (int i = 0; i < EXT_SLOTS; i++) {
		if (ext_cfg.slot[i].en) {
			return true;
		}
	}
	return false;
}
