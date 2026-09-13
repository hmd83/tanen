#ifndef TANENBASE_EXT_SENSORS_H
#define TANENBASE_EXT_SENSORS_H

#include <stdint.h>
#include <zephyr/sys/util.h>
#include "../../config/config.h"

/* Extended Mode: SwitchBot Outdoor Meter (WoIOSensorTH) readings, one entry
 * per ext_config_t slot. The types are always available (transmit.c and the
 * GATT service use them); the functions only with CONFIG_TANENBASE_EXT_SENSORS. */

#define EXT_SEEN_TH   BIT(0)  /* temperature + humidity valid */
#define EXT_SEEN_BATT BIT(1)  /* battery valid */

typedef struct {
    int64_t last_ms;  /* k_uptime_get() of the last T/H packet */
    int16_t temp_cc;  /* centi-Celsius (sensor resolution 0.1 °C) */
    uint8_t hum;      /* %RH */
    uint8_t batt;     /* % */
    int8_t  rssi;     /* dBm of the last packet */
    uint8_t seen;     /* EXT_SEEN_* */
} ext_reading_t;

typedef struct {
    ext_reading_t r[EXT_SLOTS];
} ext_data_t;

/**
 * One scan on a transmitting wake: enables BT, scans until every enabled slot
 * has sent T/H and battery or timeout_ms passes, then disables BT again.
 * @param out  readings; slots not heard have seen == 0
 * @return 0, or negative errno if BT or the scanner failed to start
 */
int ext_scan_once(ext_data_t *out, uint32_t timeout_ms);

/** SETUP mode: continuous scan (window == interval) next to the web-app link. BT must be on. */
int ext_scan_start(void);
void ext_scan_stop(void);

/** Re-read the slot config (after a GATT write); drops readings of changed MACs. */
void ext_sensors_reload(void);

/** Latest SETUP-mode readings; T/H older than 30 s is reported as not seen. */
void ext_get_cached(ext_data_t *out);

#endif
