/* TanenBase GATT service: the characteristic table and the plain
 * read/write handlers of the configuration values. Everything here runs in
 * BT RX (ATT) context and only touches ZMS through config.c — anything that
 * needs sensor or radio I/O is a ConfigCmd, handled in ble_svc_cmd.c.
 *
 * The table is the wire contract with web/index.html: characteristic order,
 * UUIDs, properties and permissions must not change without the page. */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <string.h>
#include "ble_svc_internal.h"
#include "../../config/config.h"
#include "../ext_sensors/ext_sensors.h"
#if IS_ENABLED(CONFIG_TANENBASE_TEMPCOMP)
#include "../measurement/tempcomp.h"
#endif

LOG_MODULE_DECLARE(ble_svc, LOG_LEVEL_INF);

/* Minimum MsInterval (seconds) */
#define MS_INTERVAL_MIN 60

/* ---- Generic read/write helpers for fixed-size ZMS-backed values ---- */

/* DevEUI (R/W, 8 bytes) */
static ssize_t read_dev_eui(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset)
{
    uint8_t val[8];
    config_get_dev_eui(val);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, val, sizeof(val));
}

static ssize_t write_dev_eui(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len != 8) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    config_set_dev_eui(buf);
    LOG_INF("DevEUI written via BLE");
    return len;
}

/* JoinEUI (R/W, 8 bytes) */
static ssize_t read_join_eui(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             void *buf, uint16_t len, uint16_t offset)
{
    uint8_t val[8];
    config_get_join_eui(val);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, val, sizeof(val));
}

static ssize_t write_join_eui(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len != 8) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    config_set_join_eui(buf);
    LOG_INF("JoinEUI written via BLE");
    return len;
}

/* AppKey (W only, 16 bytes) */
static ssize_t write_app_key(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len != 16) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    config_set_app_key(buf);
    LOG_INF("AppKey written via BLE");
    return len;
}

/* LoraMode — activation + data rate (R/W, 2 bytes = lora_mode_t). ABP is
 * refused until DevAddr and both session keys are stored, so the page writes
 * those first: a node switched to ABP without them could never transmit.
 * Changing mode or ABP credentials needs no session clear here — lora_init()
 * discards a stored session that does not match the config. */
static ssize_t read_lora_mode(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset)
{
    lora_mode_t m;
    config_get_lora_mode(&m);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &m, sizeof(m));
}

static ssize_t write_lora_mode(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    lora_mode_t m;

    if (offset != 0 || len != sizeof(m)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    memcpy(&m, buf, sizeof(m));
    if (config_lora_mode_validate(&m) ||
        (m.mode == LORA_MODE_ABP && !config_abp_ready())) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    if (config_set_lora_mode(&m)) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    LOG_INF("LoraMode written via BLE: %s, DR %d (-1 = ADR)",
            m.mode == LORA_MODE_ABP ? "ABP" : "OTAA",
            m.dr == LORA_DR_ADR ? -1 : m.dr);
    return len;
}

/* DevAddr (R/W, 4 bytes, MSB first as TTN prints it) — ABP */
static ssize_t read_dev_addr(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             void *buf, uint16_t len, uint16_t offset)
{
    uint8_t val[4];
    config_get_dev_addr(val);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, val, sizeof(val));
}

static ssize_t write_dev_addr(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len != 4) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    config_set_dev_addr(buf);
    LOG_INF("DevAddr written via BLE");
    return len;
}

/* NwkSKey / AppSKey (W only, 16 bytes — never readable, like AppKey) — ABP */
static ssize_t write_nwk_skey(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len != 16) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    config_set_nwk_skey(buf);
    LOG_INF("NwkSKey written via BLE");
    return len;
}

static ssize_t write_app_skey(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len != 16) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    config_set_app_skey(buf);
    LOG_INF("AppSKey written via BLE");
    return len;
}

/* ZeroOffset (R/W, int32 LE) */
static ssize_t read_zero_offset(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset)
{
    int32_t val;
    config_get_zero_offset(&val);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &val, sizeof(val));
}

static ssize_t write_zero_offset(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len != 4) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    int32_t val;
    memcpy(&val, buf, sizeof(val));
    config_set_zero_offset(val);
    LOG_INF("ZeroOffset set: %d", val);
    return len;
}

/* ScaleFactor (R/W, int32 LE) */
static ssize_t read_scale_factor(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 void *buf, uint16_t len, uint16_t offset)
{
    int32_t val;
    config_get_scale_factor(&val);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &val, sizeof(val));
}

static ssize_t write_scale_factor(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len != 4) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    int32_t val;
    memcpy(&val, buf, sizeof(val));
    config_set_scale_factor(val);
    LOG_INF("ScaleFactor set: %d", val);
    return len;
}

/* TxInterval (R/W, uint32 LE, seconds) */
static ssize_t read_tx_interval(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset)
{
    uint32_t val;
    config_get_tx_interval(&val);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &val, sizeof(val));
}

static ssize_t write_tx_interval(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len != 4) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    uint32_t val;
    memcpy(&val, buf, sizeof(val));
    uint32_t ms_int;
    config_get_ms_interval(&ms_int);
    if (val < ms_int) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    config_set_tx_interval(val);
    LOG_INF("TxInterval set: %u s", val);
    return len;
}

/* MsInterval (R/W, uint32 LE, seconds, min 60) */
static ssize_t read_ms_interval(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset)
{
    uint32_t val;
    config_get_ms_interval(&val);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &val, sizeof(val));
}

static ssize_t write_ms_interval(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len != 4) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    uint32_t val;
    memcpy(&val, buf, sizeof(val));
    uint32_t tx_int;
    config_get_tx_interval(&tx_int);
    /* TRD 7.1: reject anything violating 60s floor or T_TX >= T_MEAS */
    if (val < MS_INTERVAL_MIN || val > tx_int) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    config_set_ms_interval(val);
    LOG_INF("MsInterval set: %u s", val);
    return len;
}

/* WeightThreshold (R/W, uint16 LE, grams) */
static ssize_t read_weight_thresh(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  void *buf, uint16_t len, uint16_t offset)
{
    uint16_t val;
    config_get_anomaly_weight_threshold(&val);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &val, sizeof(val));
}

static ssize_t write_weight_thresh(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len != 2) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    uint16_t val;
    memcpy(&val, buf, sizeof(val));
    config_set_anomaly_weight_threshold(val);
    LOG_INF("WeightThreshold set: %u g", val);
    return len;
}

/* TempThreshold (R/W, uint16 LE, centi-Celsius) */
static ssize_t read_temp_thresh(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset)
{
    uint16_t val;
    config_get_anomaly_temp_threshold(&val);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &val, sizeof(val));
}

static ssize_t write_temp_thresh(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len != 2) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    uint16_t val;
    memcpy(&val, buf, sizeof(val));
    config_set_anomaly_temp_threshold(val);
    LOG_INF("TempThreshold set: %u cc", val);
    return len;
}

/* CalibRef — reference weight in grams for scale factor calibration (R/W, uint32 LE) */
uint32_t calib_ref_g;

static ssize_t read_calib_ref(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &calib_ref_g, sizeof(calib_ref_g));
}

static ssize_t write_calib_ref(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len != 4) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    memcpy(&calib_ref_g, buf, sizeof(calib_ref_g));
    LOG_INF("CalibRef set: %u g", calib_ref_g);
    return len;
}

/* ExtConfig — Extended Mode sensor slots (R/W, 26 bytes = ext_config_t).
 * The role limits are enforced here too, not only in the web page: a raw
 * GATT write must not be able to store two outside sensors. */
static ssize_t read_ext_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset)
{
    ext_config_t cfg;
    config_get_ext(&cfg);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &cfg, sizeof(cfg));
}

static ssize_t write_ext_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    ext_config_t cfg;

    if (offset != 0 || len != sizeof(cfg)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    memcpy(&cfg, buf, sizeof(cfg));
    if (config_ext_validate(&cfg)) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    if (config_set_ext(&cfg)) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
#if IS_ENABLED(CONFIG_TANENBASE_EXT_SENSORS)
    /* Live scan matches against the new MACs from the next packet on */
    ext_sensors_reload();
#endif
    LOG_INF("ExtConfig written via BLE (master %s)", cfg.enabled ? "on" : "off");
    return len;
}

/* LcProfile — load-cell profile (R/W, 10 bytes = lc_config_t).
 * Read returns the constants the correction is actually running with, so the
 * setup page can show them without carrying a copy of the profile table; only
 * LC_PROFILE_CUSTOM takes gain and tau from the write. */
static ssize_t read_lc_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset)
{
    lc_config_t cfg;
    config_get_lc(&cfg);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &cfg, sizeof(cfg));
}

static ssize_t write_lc_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    lc_config_t cfg;

    if (offset != 0 || len != sizeof(cfg)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    memcpy(&cfg, buf, sizeof(cfg));
    if (config_lc_validate(&cfg)) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    if (config_set_lc(&cfg)) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
#if IS_ENABLED(CONFIG_TANENBASE_TEMPCOMP)
    /* The live view is mid-flight — the next notify must already use the new
     * constants, otherwise picking a profile looks like it did nothing. The
     * tare survives: the correction is zero at T_ref whatever the gain. */
    tempcomp_reload();
#endif
    LOG_INF("LcProfile written via BLE: id=%u", cfg.id);
    return len;
}

/* ---- GATT Service Definition ---- */

BT_GATT_SERVICE_DEFINE(tanen_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_TANEN_SVC),

    /* DevEUI (R/W) — plain perms: Web Bluetooth has no pair() API and can't
     * reliably complete an OS-level pairing prompt triggered mid-GATT-write.
     * BT_GATT_PERM_WRITE_ENCRYPT here hung SMP and killed the link (disconnect
     * reason 8 / Connection Timeout, then bt_le_adv_start -ENOMEM). TRD 12.1's
     * "encrypted pairing required" is not achievable through this app's only
     * config path (Web Bluetooth) — see OPEN_QUESTIONS.md item 10. */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x0001),
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
        read_dev_eui, write_dev_eui, NULL),

    /* JoinEUI (R/W) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x0002),
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
        read_join_eui, write_join_eui, NULL),

    /* AppKey (W only — never readable) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x0003),
        BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_WRITE,
        NULL, write_app_key, NULL),

    /* ZeroOffset (R/W) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x0004),
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
        read_zero_offset, write_zero_offset, NULL),

    /* ScaleFactor (R/W) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x0005),
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
        read_scale_factor, write_scale_factor, NULL),

    /* TxInterval (R/W) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x0006),
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
        read_tx_interval, write_tx_interval, NULL),

    /* MsInterval (R/W) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x0007),
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
        read_ms_interval, write_ms_interval, NULL),

    /* WeightThreshold (R/W) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x0008),
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
        read_weight_thresh, write_weight_thresh, NULL),

    /* TempThreshold (R/W) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x0009),
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
        read_temp_thresh, write_temp_thresh, NULL),

    /* CalibRef — reference weight in grams for calibrate command (R/W) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x000A),
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
        read_calib_ref, write_calib_ref, NULL),

    /* CmdStatus (Notify, 2 bytes: [cmd, result]) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x000B),
        BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_NONE,
        NULL, NULL, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* ConfigCmd (W only) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x000C),
        BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_WRITE,
        NULL, write_config_cmd, NULL),

    /* LiveSensors (Notify) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x000D),
        BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_NONE,
        NULL, NULL, NULL),
    BT_GATT_CCC(live_sensor_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* ExtConfig — Extended Mode sensor slots (R/W) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x000E),
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
        read_ext_config, write_ext_config, NULL),

    /* ExtLive — BLE sensor readings (Notify) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x000F),
        BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_NONE,
        NULL, NULL, NULL),
    BT_GATT_CCC(ext_live_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* LcProfile — load-cell profile + compensation constants (R/W) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x0010),
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
        read_lc_config, write_lc_config, NULL),

    /* LoraMode — OTAA/ABP + fixed DR (R/W) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x0011),
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
        read_lora_mode, write_lora_mode, NULL),

    /* DevAddr — ABP (R/W) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x0012),
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
        read_dev_addr, write_dev_addr, NULL),

    /* NwkSKey — ABP (W only — never readable) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x0013),
        BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_WRITE,
        NULL, write_nwk_skey, NULL),

    /* AppSKey — ABP (W only — never readable) */
    BT_GATT_CHARACTERISTIC(TANEN_CHAR_UUID(0x0014),
        BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_WRITE,
        NULL, write_app_skey, NULL),
);
