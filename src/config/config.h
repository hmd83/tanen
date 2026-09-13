#ifndef TANENBASE_CONFIG_H
#define TANENBASE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

int config_init(void);

/* LoRaWAN credentials (ZMS-backed, fallback to Kconfig) */
int config_get_dev_eui(uint8_t buf[8]);
int config_get_join_eui(uint8_t buf[8]);
int config_get_app_key(uint8_t buf[16]);
int config_set_dev_eui(const uint8_t buf[8]);
int config_set_join_eui(const uint8_t buf[8]);
int config_set_app_key(const uint8_t buf[16]);

/* Monotonic DevNonce (LoRaWAN 1.0.4) — increments + persists per join attempt */
int config_get_dev_nonce(uint16_t *nonce);

/* LoRaWAN session persistence via ZMS */
int config_session_save(const void *data, size_t len);
int config_session_load(void *data, size_t len);
int config_session_clear(void);

/* LoRaWAN link-health state — persisted across System OFF so the recovery
 * ladder advances over wakes (RAM is wiped each cycle). */
typedef struct {
	uint16_t uplink_count;  /* uplinks since boot-of-counter; probe cadence */
	uint8_t  link_fail;     /* consecutive confirmed-probe no-ACKs */
	uint8_t  join_fail;     /* consecutive OTAA join failures */
	int8_t   forced_dr;     /* -1 = ADR/healthy; >=0 = pinned DR (degraded) */
} link_state_t;

int config_get_link_state(link_state_t *st);
int config_set_link_state(const link_state_t *st);

/* Weight calibration */
int config_get_zero_offset(int32_t *val);
int config_set_zero_offset(int32_t val);
int config_get_scale_factor(int32_t *val);
int config_set_scale_factor(int32_t val);

/* NAU7802 OCAL1 (24-bit signed). Saved once on cold cal so deep-sleep
 * power-down (PUD=0) can restore it on wake → no re-cal, baseline stable. */
int config_get_ocal1(int32_t *val);
int config_set_ocal1(int32_t val);

/* Load-cell temperature compensation — the thermal-lag filter has a 25 min
 * time constant but RAM is wiped every System OFF, so t_eff must survive the
 * sleep or the filter re-seeds every cycle and never lags anything. */
typedef struct {
	int32_t t_eff_mdeg;  /* filtered cell-body temperature [m°C] */
	uint8_t primed;      /* 0 = re-seed from the next sample */
} tempcomp_state_t;

int config_get_tempcomp_state(tempcomp_state_t *st);
int config_set_tempcomp_state(const tempcomp_state_t *st);

/* Tare temperature (T_ref of the correction) in milli-Celsius — captured by
 * the BLE tare command, since the correction is anchored to whatever
 * temperature the scale was zeroed at. */
int config_get_tare_temp(int32_t *val);
int config_set_tare_temp(int32_t val);

/* Timing (seconds) */
int config_get_tx_interval(uint32_t *val);
int config_set_tx_interval(uint32_t val);
int config_get_ms_interval(uint32_t *val);
int config_set_ms_interval(uint32_t val);

/* Anomaly thresholds */
int config_get_anomaly_weight_threshold(uint16_t *val);
int config_set_anomaly_weight_threshold(uint16_t val);
int config_get_anomaly_temp_threshold(uint16_t *val);
int config_set_anomaly_temp_threshold(uint16_t val);

/* Delta tracking (last transmitted values) */
int config_get_last_tx_weight(uint32_t *val);
int config_set_last_tx_weight(uint32_t val);
int config_get_last_tx_temp(int16_t *val);
int config_set_last_tx_temp(int16_t val);

/* Measurement cycle counter */
int config_get_measurement_count(uint32_t *val);
int config_set_measurement_count(uint32_t val);

/* TX-pending retry flag — set when a due uplink failed (join/radio error) so
 * the next wake retries instead of staying silent until the next heartbeat. */
int config_get_tx_pending(uint8_t *val);
int config_set_tx_pending(uint8_t val);

/* Extended Mode — up to EXT_SLOTS SwitchBot Outdoor Meter BLE sensors.
 * All-uint8 layout, so the struct is the ZMS record and the ExtConfig GATT
 * value byte for byte (26 bytes, no padding). */
#define EXT_SLOTS           3
#define EXT_CONFIG_VERSION  1
#define EXT_ROLE_IN         0  /* in-hive */
#define EXT_ROLE_OUT        1  /* out-of-hive: replaces the 1-Wire outside temp */
#define EXT_MAX_IN          2
#define EXT_MAX_OUT         1

typedef struct {
	uint8_t mac[6];  /* MSB first, as printed on the label */
	uint8_t role;    /* EXT_ROLE_* */
	uint8_t en;      /* 0/1 */
} ext_slot_t;

typedef struct {
	uint8_t    version;  /* EXT_CONFIG_VERSION */
	uint8_t    enabled;  /* master switch 0/1 */
	ext_slot_t slot[EXT_SLOTS];
} ext_config_t;

int config_get_ext(ext_config_t *cfg);
int config_set_ext(const ext_config_t *cfg);
/* 0 if storable: known version, 0/1 flags, every enabled slot has a unique
 * non-zero MAC, at most EXT_MAX_IN in-hive and EXT_MAX_OUT outside. */
int config_ext_validate(const ext_config_t *cfg);
/* Master on and at least one slot enabled — a transmitting wake must scan. */
bool config_ext_active(void);

#endif
