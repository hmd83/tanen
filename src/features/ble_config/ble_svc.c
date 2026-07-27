#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>
#include <stdio.h>
#include <string.h>
#include "ble_svc.h"
#include "../../config/config.h"
#include "../../core/watchdog.h"
#include "../../drivers/lora.h"
#include "../measurement/measure.h"

LOG_MODULE_REGISTER(ble_svc, LOG_LEVEL_INF);

/* Event flags */
#define EVT_CONFIG_DONE BIT(0)

static K_EVENT_DEFINE(config_evt);

/* active_conn is touched from three contexts: BT RX (conn callbacks),
 * sysWQ (notify work) and the main thread (shutdown). Unref races =
 * use-after-free on struct bt_conn. All access goes through conn_mut;
 * users take their own ref via conn_acquire() and release it after use. */
static K_MUTEX_DEFINE(conn_mut);
static struct bt_conn *active_conn;
static bool shutting_down;

static struct bt_conn *conn_acquire(void)
{
    struct bt_conn *c = NULL;

    k_mutex_lock(&conn_mut, K_FOREVER);
    if (active_conn) {
        c = bt_conn_ref(active_conn);
    }
    k_mutex_unlock(&conn_mut);
    return c;
}

/* Custom service UUID: 544E4253-0000-4269-8000-544E42415345 */
#define BT_UUID_TANEN_SVC_VAL \
    BT_UUID_128_ENCODE(0x544E4253, 0x0000, 0x4269, 0x8000, 0x544E42415345)
#define BT_UUID_TANEN_SVC BT_UUID_DECLARE_128(BT_UUID_TANEN_SVC_VAL)

/* Characteristic UUIDs: base with incrementing byte 5-6 */
#define TANEN_CHAR_UUID(n) \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x544E4253, (n), 0x4269, 0x8000, 0x544E42415345))

/* ConfigCmd values */
#define CMD_DONE          0x01
#define CMD_TARE          0x02
#define CMD_CLEAR_SESSION 0x03
#define CMD_TEST_LORA     0x04
#define CMD_CALIBRATE     0x05

/* Pending command for deferred execution in work queue. atomic CAS gate:
 * a second command while one is in flight is rejected (previously it
 * silently overwrote the first — lost command, confused web app). */
static atomic_t pending_cmd = ATOMIC_INIT(0);

/* Set while the LoRa test thread owns the radio — live-sensor work must
 * not hog the sysWQ then (LoRaMAC RX-window timers run there; a 2-3 s
 * blocking measure_run makes the JoinAccept window get missed). */
static atomic_t lora_test_running = ATOMIC_INIT(0);

/* CmdStatus notify — sends [cmd, result] after async commands complete */
static const struct bt_gatt_attr *cmd_status_attr;

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
static uint32_t calib_ref_g;

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

/* ConfigCmd (W only, 1 byte) — commands that need sensor I/O are deferred to work queue */
static ssize_t write_config_cmd(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len != 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    uint8_t cmd = ((const uint8_t *)buf)[0];

    switch (cmd) {
    case CMD_DONE:
        LOG_INF("BLE cmd: DONE");
        k_event_set(&config_evt, EVT_CONFIG_DONE);
        break;
    case CMD_TARE:
    case CMD_CALIBRATE:
    case CMD_TEST_LORA:
        if (!atomic_cas(&pending_cmd, 0, cmd)) {
            LOG_WRN("BLE cmd 0x%02x rejected — 0x%02x still running",
                    cmd, (uint8_t)atomic_get(&pending_cmd));
            return BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED);
        }
        LOG_INF("BLE cmd: 0x%02x (deferred)", cmd);
        extern struct k_work cmd_work;
        k_work_submit(&cmd_work);
        break;
    case CMD_CLEAR_SESSION:
        LOG_INF("BLE cmd: CLEAR SESSION");
        config_session_clear();
        break;
    default:
        LOG_WRN("Unknown BLE cmd: 0x%02x", cmd);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    return len;
}

/* Forward declaration — timer defined after GATT table */
static void live_timer_expiry(struct k_timer *timer);
static K_TIMER_DEFINE(live_timer, live_timer_expiry, NULL);

/* LiveSensors (Notify, 8 bytes: uint32 weight_g + int16 temp_cc + uint16 batt_mv) */
static void live_sensor_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    LOG_INF("LiveSensors notify %s", value ? "enabled" : "disabled");
    if (value) {
        k_timer_start(&live_timer, K_SECONDS(2), K_SECONDS(2));
    } else {
        k_timer_stop(&live_timer);
    }
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
);

/* ---- Advertising ---- */

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_TANEN_SVC_VAL),
};

/* Scan response — device name so Web Bluetooth shows "TanenBase-XXXX" instead
 * of MAC. XXXX = last 4 hex of DevEUI, filled at runtime (see ble_set_name). */
static char dev_name[sizeof("TanenBase-XXXX")] = CONFIG_BT_DEVICE_NAME;
static struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, dev_name, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* Build "TanenBase-XXXX" from the last 2 DevEUI bytes and register it as the
 * GAP name + scan-response payload. Call before bt_le_adv_start. */
static void ble_set_name(void)
{
    uint8_t eui[8];

    config_get_dev_eui(eui);
    snprintf(dev_name, sizeof(dev_name), "TanenBase-%02X%02X", eui[6], eui[7]);
    sd[0].data_len = strlen(dev_name);
    bt_set_name(dev_name);
    LOG_INF("BLE name: %s", dev_name);
}

/* ---- Connection callbacks ---- */

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("BLE connect failed: %d", err);
        return;
    }
    k_mutex_lock(&conn_mut, K_FOREVER);
    active_conn = bt_conn_ref(conn);
    k_mutex_unlock(&conn_mut);
    LOG_INF("BLE connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("BLE disconnected (reason %u)", reason);
    k_mutex_lock(&conn_mut, K_FOREVER);
    if (active_conn) {
        bt_conn_unref(active_conn);
        active_conn = NULL;
    }
    k_mutex_unlock(&conn_mut);
    /* Re-advertise so user can reconnect (but not during shutdown) */
    if (!shutting_down) {
        int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
        if (err) {
            LOG_ERR("Re-advertise failed: %d", err);
        } else {
            LOG_INF("Re-advertising");
        }
    }
}

BT_CONN_CB_DEFINE(conn_cbs) = {
    .connected = connected,
    .disconnected = disconnected,
};

/* ---- Live sensor notification (work queue — sensor reads need thread context) ---- */

static const struct bt_gatt_attr *live_sensor_attr;

static void live_sensor_work_handler(struct k_work *work)
{
    /* Never stall the sysWQ while the LoRa test owns the radio — LoRaMAC
     * RX-window timers run on this queue and a 2-3 s measure_run would
     * make the node miss its JoinAccept/ACK windows. */
    if (atomic_get(&lora_test_running) || !live_sensor_attr) {
        return;
    }

    struct bt_conn *conn = conn_acquire();
    if (!conn) {
        return;
    }

    measurement_data_t data;
    int err = measure_run(&data);
    if (!err) {
        /* 8 bytes LE: weight_g(u32) + temp_cc(i16) + battery_mv(u16) */
        uint8_t buf[8];
        sys_put_le32(data.weight_g, &buf[0]);
        sys_put_le16((uint16_t)data.temp_cc, &buf[4]);
        sys_put_le16(data.battery_mv, &buf[6]);

        err = bt_gatt_notify(conn, live_sensor_attr, buf, sizeof(buf));
        if (err && err != -ENOTCONN) {
            LOG_WRN("live notify failed: %d", err);
        }
    }
    bt_conn_unref(conn);
}

static K_WORK_DEFINE(live_sensor_work, live_sensor_work_handler);

/* ---- Deferred command handler (sensor/LoRa I/O — can't run in BLE ATT context) ---- */

static void notify_cmd_status(uint8_t cmd, uint8_t result)
{
    if (!cmd_status_attr) {
        return;
    }
    struct bt_conn *conn = conn_acquire();
    if (!conn) {
        return;
    }
    uint8_t buf[2] = {cmd, result};
    (void)bt_gatt_notify(conn, cmd_status_attr, buf, sizeof(buf));
    bt_conn_unref(conn);
}

static void cmd_work_handler(struct k_work *work)
{
    extern int weight_read(int32_t *raw);
    int32_t raw;
    int err;

    uint8_t cmd = (uint8_t)atomic_get(&pending_cmd);

    switch (cmd) {
    case CMD_TARE:
        err = weight_read(&raw);
        if (!err) {
            config_set_zero_offset(raw);
            LOG_INF("Tare done: offset=%d", raw);
        } else {
            LOG_ERR("Tare failed: %d", err);
        }
        notify_cmd_status(cmd, err ? 1 : 0);
        break;
    case CMD_CALIBRATE:
        if (calib_ref_g == 0) {
            LOG_ERR("Calibrate: ref weight is 0");
            notify_cmd_status(cmd, 2);
            break;
        }
        err = weight_read(&raw);
        if (!err) {
            int32_t offset;
            config_get_zero_offset(&offset);
            int32_t delta = raw - offset;
            if (delta == 0) {
                LOG_ERR("Calibrate: raw==offset, no load?");
                notify_cmd_status(cmd, 3);
                break;
            }
            int32_t sf = (delta * 1000) / (int32_t)calib_ref_g;
            config_set_scale_factor(sf);
            LOG_INF("Calibrate done: raw=%d offset=%d ref=%ug sf=%d", raw, offset, calib_ref_g, sf);
        } else {
            LOG_ERR("Calibrate failed: %d", err);
        }
        notify_cmd_status(cmd, err ? 1 : 0);
        break;
    case CMD_TEST_LORA:
        /* LoRa test runs on dedicated thread — lorawan_send blocks waiting
         * for TX+RX timer callbacks on system work queue, so running it ON
         * the work queue deadlocks. pending_cmd stays set until the test
         * thread exits (gates further commands off the radio). */
        LOG_INF("LoRa test: spawning thread...");
        k_timer_stop(&live_timer);
        extern void lora_test_thread_start(void);
        lora_test_thread_start();
        return;
    }

    atomic_clear(&pending_cmd);
}

K_WORK_DEFINE(cmd_work, cmd_work_handler);

/* ---- LoRa test thread (lorawan_send blocks on work queue timers — needs own thread) ---- */

static void lora_test_entry(void *p1, void *p2, void *p3)
{
    int err;

    atomic_set(&lora_test_running, 1);

    LOG_INF("LoRa test: init + join...");
    err = lora_init();
    if (err) {
        LOG_ERR("LoRa init failed: %d", err);
        notify_cmd_status(CMD_TEST_LORA, 1);
        goto out;
    }

    uint8_t test_payload[] = {0xFF};
    err = lora_send(test_payload, sizeof(test_payload), false);
    if (err) {
        LOG_ERR("LoRa test send failed: %d", err);
    } else {
        LOG_INF("LoRa test uplink sent");
    }

    lora_session_save();
    /* sx12xx_common puts radio back in sleep on TX done */
    notify_cmd_status(CMD_TEST_LORA, err ? 2 : 0);

out:
    atomic_set(&lora_test_running, 0);
    atomic_clear(&pending_cmd);
}

#define LORA_TEST_STACK_SIZE 4096
static K_THREAD_STACK_DEFINE(lora_test_stack, LORA_TEST_STACK_SIZE);
static struct k_thread lora_test_thread;
static bool lora_test_started;

void lora_test_thread_start(void)
{
    lora_test_started = true;
    k_thread_create(&lora_test_thread, lora_test_stack, LORA_TEST_STACK_SIZE,
                    lora_test_entry, NULL, NULL, NULL,
                    K_PRIO_COOP(7), 0, K_NO_WAIT);
}

static void live_timer_expiry(struct k_timer *timer)
{
    k_work_submit(&live_sensor_work);
}

/* ---- Public API ---- */

int ble_config_run(uint32_t timeout_s)
{
    int err;

    k_event_clear(&config_evt, EVT_CONFIG_DONE);
    shutting_down = false;
    lora_test_started = false;
    atomic_clear(&pending_cmd);
    atomic_clear(&lora_test_running);

    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("bt_enable failed: %d", err);
        return err;
    }

    /* Append DevEUI suffix to advertised name (needs bt_enable first) */
    ble_set_name();

    /* Find notify characteristic attrs */
    live_sensor_attr = bt_gatt_find_by_uuid(tanen_svc.attrs, tanen_svc.attr_count,
                                            TANEN_CHAR_UUID(0x000D));
    cmd_status_attr = bt_gatt_find_by_uuid(tanen_svc.attrs, tanen_svc.attr_count,
                                           TANEN_CHAR_UUID(0x000B));

    err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err) {
        LOG_ERR("Advertising start failed: %d", err);
        bt_disable();
        return err;
    }

    LOG_INF("BLE advertising started (timeout %u s)", timeout_s);

    /* Wait for done command or timeout — in ≤10s slices so the 180s SETUP
     * window doesn't starve the 60s watchdog. */
    for (uint32_t waited = 0; waited < timeout_s; waited += 10) {
        uint32_t ev = k_event_wait(&config_evt, EVT_CONFIG_DONE, false,
                                   K_SECONDS(MIN(10U, timeout_s - waited)));
        watchdog_feed();
        if (ev & EVT_CONFIG_DONE) {
            break;
        }
    }

    /* Stop live sensor timer */
    k_timer_stop(&live_timer);

    LOG_INF("BLE config exiting — cleaning up");
    shutting_down = true;

    /* Wait for LoRa test thread if it was started — sliced for WDT feeds.
     * If it is still stuck after 30s the radio/SPI is wedged; do NOT abort
     * the thread mid-SPI (bus state undefined) — proceed to shutdown and
     * let the watchdog reset if it never unblocks. */
    if (lora_test_started) {
        int jrc = -EAGAIN;
        for (int i = 0; i < 6 && jrc != 0; i++) {
            jrc = k_thread_join(&lora_test_thread, K_SECONDS(5));
            watchdog_feed();
        }
        if (jrc) {
            LOG_ERR("LoRa test thread did not exit: %d", jrc);
        }
    }

    /* BLE shutdown:
     * 1. Stop advertising first (prevents new connections)
     * 2. Disconnect active connection
     * 3. Wait for disconnect callback to complete
     * 4. Disable BLE stack */
    bt_le_adv_stop();

    struct bt_conn *conn = conn_acquire();
    if (conn) {
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(conn);
        /* Wait for disconnect callback — it drops the active_conn ref under
         * conn_mut (no double-unref race with this thread anymore). */
        k_sleep(K_MSEC(500));
        k_mutex_lock(&conn_mut, K_FOREVER);
        if (active_conn) {
            /* Callback never fired (controller wedged) — drop ref here */
            bt_conn_unref(active_conn);
            active_conn = NULL;
        }
        k_mutex_unlock(&conn_mut);
    }

    /* Small delay to let controller settle before HCI_Reset */
    k_sleep(K_MSEC(100));
    bt_disable();

    LOG_INF("BLE disabled");
    return 0;
}
