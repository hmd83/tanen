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
#include "ble_svc_internal.h"
#include "../../config/config.h"
#include "../../core/watchdog.h"
#include "../measurement/measure.h"
#include "../ext_sensors/ext_sensors.h"

LOG_MODULE_REGISTER(ble_svc, LOG_LEVEL_INF);

K_EVENT_DEFINE(config_evt);

/* active_conn is touched from three contexts: BT RX (conn callbacks),
 * sysWQ (notify work) and the main thread (shutdown). Unref races =
 * use-after-free on struct bt_conn. All access goes through conn_mut;
 * users take their own ref via conn_acquire() and release it after use. */
static K_MUTEX_DEFINE(conn_mut);
static struct bt_conn *active_conn;
static bool shutting_down;

struct bt_conn *conn_acquire(void)
{
    struct bt_conn *c = NULL;

    k_mutex_lock(&conn_mut, K_FOREVER);
    if (active_conn) {
        c = bt_conn_ref(active_conn);
    }
    k_mutex_unlock(&conn_mut);
    return c;
}

/* Sensor I/O triggered over BLE (live readings, tare, calibrate) runs on its
 * own PREEMPTIBLE queue, never the system workqueue. The sysWQ is cooperative
 * (prio -1): no other thread runs until a work item blocks. gpio-I2C does not
 * block — i2c_bitbang's i2c_delay() is a spin loop — and with the NAU7802
 * absent or unpowered every SCL edge spins out the 100 ms clock-stretch
 * timeout: ~3.5 s of pure spin per measure_run(), re-queued every 2 s. On the
 * sysWQ that starved the BT RX thread (GATT stalled, the web app dropped after
 * ~5 s) and main (watchdog reset 60 s later, then no SETUP on the reset wake).
 * Below main's priority the watchdog feed and the BT host preempt the spin. */
#define SENSOR_WQ_STACK_SIZE 4096
#define SENSOR_WQ_PRIO       K_PRIO_PREEMPT(10)
static K_THREAD_STACK_DEFINE(sensor_wq_stack, SENSOR_WQ_STACK_SIZE);
struct k_work_q sensor_wq;

/* Forward declaration — timer defined after GATT table */
static void live_timer_expiry(struct k_timer *timer);
K_TIMER_DEFINE(live_timer, live_timer_expiry, NULL);

/* One 2 s timer drives both live notifies; each work item checks its own
 * subscription, so ExtLive alone never triggers a 2-3 s measure_run. */
static atomic_t live_sensor_on = ATOMIC_INIT(0);
static atomic_t ext_live_on = ATOMIC_INIT(0);

static void live_timer_update(void)
{
    if (atomic_get(&live_sensor_on) || atomic_get(&ext_live_on)) {
        k_timer_start(&live_timer, K_SECONDS(2), K_SECONDS(2));
    } else {
        k_timer_stop(&live_timer);
    }
}

/* LiveSensors (Notify, 8 bytes: uint32 weight_g + int16 temp_cc + uint16 batt_mv) */
void live_sensor_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    LOG_INF("LiveSensors notify %s", value ? "enabled" : "disabled");
    atomic_set(&live_sensor_on, value ? 1 : 0);
    live_timer_update();
}

/* ExtLive (Notify, 18 bytes: 3 x {status, int16 temp_cc, hum, batt, int8 rssi}) */
void ext_live_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    LOG_INF("ExtLive notify %s", value ? "enabled" : "disabled");
    atomic_set(&ext_live_on, value ? 1 : 0);
    live_timer_update();
}

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
    /* Stay off the sensors while the LoRa test thread runs its own
     * measure_run() — two NAU7802 sequences would interleave on the bus. */
    if (atomic_get(&lora_test_running) || !atomic_get(&live_sensor_on) || !live_sensor_attr) {
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

/* ExtLive — cached scan results only, no sensor I/O, so it never stalls the
 * sysWQ. 6 bytes LE per slot: status (EXT_SEEN_*), temp_cc, hum, batt, rssi.
 * 18 bytes fit the 20-byte notify of the default 23-byte ATT MTU. */
static const struct bt_gatt_attr *ext_live_attr;

static void ext_live_work_handler(struct k_work *work)
{
    if (!atomic_get(&ext_live_on) || !ext_live_attr) {
        return;
    }

    struct bt_conn *conn = conn_acquire();
    if (!conn) {
        return;
    }

    ext_data_t ext = {0};
#if IS_ENABLED(CONFIG_TANENBASE_EXT_SENSORS)
    ext_get_cached(&ext);
#endif
    uint8_t buf[EXT_SLOTS * 6];

    for (int i = 0; i < EXT_SLOTS; i++) {
        const ext_reading_t *r = &ext.r[i];
        uint8_t *p = &buf[i * 6];

        p[0] = r->seen;
        sys_put_le16((uint16_t)r->temp_cc, &p[1]);
        p[3] = r->hum;
        p[4] = r->batt;
        p[5] = (uint8_t)r->rssi;
    }

    int err = bt_gatt_notify(conn, ext_live_attr, buf, sizeof(buf));
    if (err && err != -ENOTCONN) {
        LOG_WRN("ext live notify failed: %d", err);
    }
    bt_conn_unref(conn);
}

static K_WORK_DEFINE(ext_live_work, ext_live_work_handler);

static void live_timer_expiry(struct k_timer *timer)
{
    /* Skip a tick while the last read is still queued or running — with a
     * dead sensor one read outlasts the 2 s period and would chain
     * back-to-back. */
    if (!k_work_is_pending(&live_sensor_work)) {
        k_work_submit_to_queue(&sensor_wq, &live_sensor_work);
    }
    k_work_submit(&ext_live_work);
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

    static bool sensor_wq_started;
    if (!sensor_wq_started) {
        static const struct k_work_queue_config sensor_wq_cfg = { .name = "sensor_wq" };

        k_work_queue_start(&sensor_wq, sensor_wq_stack, K_THREAD_STACK_SIZEOF(sensor_wq_stack),
                           SENSOR_WQ_PRIO, &sensor_wq_cfg);
        sensor_wq_started = true;
    }

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
    ext_live_attr = bt_gatt_find_by_uuid(tanen_svc.attrs, tanen_svc.attr_count,
                                         TANEN_CHAR_UUID(0x000F));

    err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err) {
        LOG_ERR("Advertising start failed: %d", err);
        bt_disable();
        return err;
    }

    LOG_INF("BLE advertising started (timeout %u s)", timeout_s);

#if IS_ENABLED(CONFIG_TANENBASE_EXT_SENSORS)
    /* Live BLE sensor values for the web app. Runs regardless of the master
     * switch so a sensor shows up before the user turns it on. Not fatal. */
    (void)ext_scan_start();
#endif

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

    /* Stop live sensor timer, then let a read already queued finish — it
     * must not race the MEASUREMENT cycle main starts right after SETUP.
     * Bounded by one dead-sensor read (~3.5 s), well inside the watchdog. */
    k_timer_stop(&live_timer);
    k_work_queue_drain(&sensor_wq, false);
    watchdog_feed();

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
#if IS_ENABLED(CONFIG_TANENBASE_EXT_SENSORS)
    ext_scan_stop();
#endif
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
