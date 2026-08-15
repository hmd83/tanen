#ifndef TANENBASE_CONFIG_H
#define TANENBASE_CONFIG_H

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

#endif
