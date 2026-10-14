/* ConfigCmd: the one-byte command characteristic, its parser, and the
 * commands it defers — tare, calibrate and the LoRa test need sensor or radio
 * I/O, which cannot run in BT RX (ATT) context. Tare and calibrate run on
 * sensor_wq; the LoRa test gets its own thread (see lora_test_entry). */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/conn.h>
#include "ble_svc_internal.h"
#include "../../config/config.h"
#include "../../drivers/lora.h"
#include "../measurement/measure.h"
#include "../transmission/transmit.h"
#include "../ext_sensors/ext_sensors.h"
#if IS_ENABLED(CONFIG_TANENBASE_TEMPCOMP)
#include "../measurement/tempcomp.h"
#endif

LOG_MODULE_DECLARE(ble_svc, LOG_LEVEL_INF);

/* ConfigCmd values */
#define CMD_DONE          0x01
#define CMD_TARE          0x02
#define CMD_CLEAR_SESSION 0x03
#define CMD_TEST_LORA     0x04
#define CMD_CALIBRATE     0x05

/* Pending command for deferred execution in work queue. atomic CAS gate:
 * a second command while one is in flight is rejected (previously it
 * silently overwrote the first — lost command, confused web app). */
atomic_t pending_cmd = ATOMIC_INIT(0);

/* Set while the LoRa test thread owns the radio — live-sensor work must
 * not hog the sysWQ then (LoRaMAC RX-window timers run there; a 2-3 s
 * blocking measure_run makes the JoinAccept window get missed). */
atomic_t lora_test_running = ATOMIC_INIT(0);

/* CmdStatus notify — sends [cmd, result] after async commands complete */
const struct bt_gatt_attr *cmd_status_attr;

/* ConfigCmd (W only, 1 byte) — commands that need sensor I/O are deferred to work queue */
ssize_t write_config_cmd(struct bt_conn *conn, const struct bt_gatt_attr *attr,
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
        k_work_submit_to_queue(&sensor_wq, &cmd_work);
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
    case CMD_TARE: {
        uint8_t result = 0;

        err = weight_read(&raw);
        if (!err) {
            config_set_zero_offset(raw);
            LOG_INF("Tare done: offset=%d", raw);
#if IS_ENABLED(CONFIG_TANENBASE_TEMPCOMP)
            /* Re-anchor the temperature correction on the same instant. Report
             * a distinct failure if that half did not land: the offset is
             * stored but the scale will read k_c * (T - T_ref) instead of
             * zero, which looks like a broken tare rather than a dead probe. */
            if (tempcomp_tare()) {
                result = 4;
            }
#endif
        } else {
            LOG_ERR("Tare failed: %d", err);
            result = 1;
        }
        notify_cmd_status(cmd, result);
        break;
    }
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

    /* Sensors first, radio second: measure_run takes 2-3 s and would
     * otherwise land inside a JoinAccept/RX window. A total sensor
     * failure is not fatal here — valid=0 makes transmit_run emit the
     * all-sentinel frame, which still decodes cleanly. */
    measurement_data_t data = {0};
    if (measure_run(&data)) {
        LOG_WRN("LoRa test: all sensors failed, sending sentinel frame");
        data.valid = 0;
    }

    LOG_INF("LoRa test: init + join...");
    lora_set_manual(true);   /* installer present: no SF12 backoff/skip */
    err = lora_init();
    if (err) {
        LOG_ERR("LoRa init failed: %d", err);
        notify_cmd_status(CMD_TEST_LORA, 1);
        goto out;
    }

    /* Same frame as a normal uplink (flags=0 -> unconfirmed).
     * A short test frame gets rejected by the TTN decoder and shows up
     * as a bogus 255 on the platform. With Extended Mode on, the live
     * scan's latest readings ride along, so the test also exercises the
     * decoder's BLE blocks. */
    const ext_data_t *ext_p = NULL;
#if IS_ENABLED(CONFIG_TANENBASE_EXT_SENSORS)
    ext_data_t ext;

    if (config_ext_active()) {
        ext_get_cached(&ext);
        ext_p = &ext;
    }
#endif
    err = transmit_run(&data, ext_p, 0);
    if (err) {
        LOG_ERR("LoRa test send failed: %d", err);
    } else {
        LOG_INF("LoRa test uplink sent");
    }

    lora_session_save();
    /* sx12xx_common puts radio back in sleep on TX done */
    notify_cmd_status(CMD_TEST_LORA, err ? 2 : 0);

out:
    lora_set_manual(false);   /* a later FSM TX this boot is routine again */
    atomic_set(&lora_test_running, 0);
    atomic_clear(&pending_cmd);
}

#define LORA_TEST_STACK_SIZE 4096
static K_THREAD_STACK_DEFINE(lora_test_stack, LORA_TEST_STACK_SIZE);
struct k_thread lora_test_thread;
bool lora_test_started;

void lora_test_thread_start(void)
{
    lora_test_started = true;
    k_thread_create(&lora_test_thread, lora_test_stack, LORA_TEST_STACK_SIZE,
                    lora_test_entry, NULL, NULL, NULL,
                    K_PRIO_COOP(7), 0, K_NO_WAIT);
}
