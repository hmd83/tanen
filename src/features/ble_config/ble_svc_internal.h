/* Private to src/features/ble_config/ — not part of the module's API (that is
 * ble_svc.h). The BLE config service is split three ways:
 *
 *   ble_svc.c       connection lifecycle, advertising, live notifies,
 *                   ble_config_run()
 *   ble_svc_gatt.c  GATT table + plain config characteristic handlers
 *   ble_svc_cmd.c   ConfigCmd parser, deferred commands, LoRa test thread
 *
 * What they share is declared here. Most of it is touched from more than one
 * execution context — BT RX (ATT handlers, conn callbacks), the sysWQ and
 * sensor_wq (notify/command work), the LoRa test thread and main (SETUP
 * entry/exit) — which is why each entry names its owner and its rules.
 */
#ifndef TANENBASE_BLE_SVC_INTERNAL_H
#define TANENBASE_BLE_SVC_INTERNAL_H

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

/* Custom service UUID: 544E4253-0000-4269-8000-544E42415345 */
#define BT_UUID_TANEN_SVC_VAL \
    BT_UUID_128_ENCODE(0x544E4253, 0x0000, 0x4269, 0x8000, 0x544E42415345)
#define BT_UUID_TANEN_SVC BT_UUID_DECLARE_128(BT_UUID_TANEN_SVC_VAL)

/* Characteristic UUIDs: base with incrementing byte 5-6 */
#define TANEN_CHAR_UUID(n) \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x544E4253, (n), 0x4269, 0x8000, 0x544E42415345))

/* ---- ble_svc_gatt.c ---- */

/* The service (BT_GATT_SERVICE_DEFINE). ble_config_run() looks the notify
 * attributes up in it by UUID. */
extern const struct bt_gatt_service_static tanen_svc;

/* CalibRef value — written over GATT, consumed by CMD_CALIBRATE. RAM only,
 * deliberately not persisted: it is an input to one calibration. */
extern uint32_t calib_ref_g;

/* ---- ble_svc_cmd.c ---- */

/* ConfigCmd write handler, referenced by the GATT table. */
ssize_t write_config_cmd(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags);

/* In-flight deferred command (0 = none); atomic CAS gate, cleared by the
 * command when it finishes and by ble_config_run() on SETUP entry. */
extern atomic_t pending_cmd;

/* Set while the LoRa test thread owns the radio and its own measure_run():
 * the live-sensor work stays off the sensors and the sysWQ meanwhile. */
extern atomic_t lora_test_running;

/* CmdStatus notify attribute, resolved by ble_config_run(). */
extern const struct bt_gatt_attr *cmd_status_attr;

/* LoRa test thread — ble_config_run() joins it before BLE shutdown. */
extern struct k_thread lora_test_thread;
extern bool lora_test_started;

/* ---- ble_svc.c ---- */

/* CMD_DONE sets EVT_CONFIG_DONE; ble_config_run() waits on it. */
#define EVT_CONFIG_DONE BIT(0)
extern struct k_event config_evt;

/* Take a reference on the active connection, or NULL. Release with
 * bt_conn_unref(). The only safe way to reach the connection from work items
 * and threads — see conn_mut in ble_svc.c. */
struct bt_conn *conn_acquire(void);

/* Preemptible queue for sensor I/O triggered over BLE — see ble_svc.c for why
 * this must never be the sysWQ. */
extern struct k_work_q sensor_wq;

/* 2 s live-notify timer; the LoRa test stops it while it owns the radio. */
extern struct k_timer live_timer;

/* CCC callbacks of LiveSensors / ExtLive, referenced by the GATT table. */
void live_sensor_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
void ext_live_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

#endif
