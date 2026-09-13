#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/sys/byteorder.h>
#include "ext_sensors.h"
#include "../../core/watchdog.h"

LOG_MODULE_REGISTER(ext_sensors, LOG_LEVEL_INF);

/* SwitchBot Outdoor Meter advertising, see
 * BLE_Thermometer_Hygrometer/SwitchBot_Outdoor_Meter_BLE_Format.md */
#define SB_COMPANY_ID     0x0969  /* Woan Technology, AD 0xFF */
#define SB_SVC_UUID       0xFD3D  /* AD 0x16 */
#define SB_MODEL_OUTDOOR  0x77
#define SB_MFR_MIN_LEN    13      /* company(2) mac(6) status(2) frac int hum */
#define SB_SVC_MIN_LEN    5       /* uuid(2) model status batt */

/* A SETUP-mode reading older than this shows as "not found" */
#define FRESH_MS          30000

/* Scan timing in 0.625 ms units — window == interval in BOTH modes, on
 * purpose. The SoftDevice Controller raises a scanner whose window is shorter
 * than its interval to 2nd scheduling priority once it misses full windows,
 * i.e. above a peripheral connection (3rd), where it can steal the web-app
 * link's events. A scanner with window == interval runs at 4th priority and
 * is interleaved around connection and advertising events instead (nrfxlib
 * softdevice_controller/doc/scheduling.rst, "Scheduling priorities"). Continuous RX only costs power
 * inside the 180 s SETUP window and the ≤8 s TX scan. */
#define SCAN_INTERVAL  0x0060  /* 60 ms */
#define SCAN_WINDOW    0x0060  /* 60 ms */

BUILD_ASSERT(SCAN_WINDOW == SCAN_INTERVAL,
             "window < interval lets the scanner pre-empt the web-app link");

/* device_found() runs in the BT RX thread; ext_get_cached() and
 * ext_scan_once() read from other threads. */
static struct k_spinlock lock;
static ext_config_t cfg;       /* slot snapshot the scan matches against */
static ext_data_t cache;
static bool signal_all_seen;   /* a TX scan is waiting for every enabled slot */
static K_SEM_DEFINE(all_seen_sem, 0, 1);

struct sb_adv {
    bool    th;
    bool    batt;
    bool    has_mac;
    int16_t temp_cc;
    uint8_t hum;
    uint8_t batt_pct;
    uint8_t mac[6];  /* MSB first, from the manufacturer data */
};

static bool parse_ad(struct bt_data *d, void *user_data)
{
    struct sb_adv *a = user_data;

    if (d->type == BT_DATA_MANUFACTURER_DATA && d->data_len >= SB_MFR_MIN_LEN &&
        sys_get_le16(d->data) == SB_COMPANY_ID) {
        int16_t cc = (int16_t)((d->data[11] & 0x7F) * 100 + (d->data[10] & 0x0F) * 10);

        /* bit 7 of the integer byte set = positive */
        a->temp_cc = (d->data[11] & 0x80) ? cc : -cc;
        a->hum = MIN(d->data[12] & 0x7F, 100);
        memcpy(a->mac, &d->data[2], sizeof(a->mac));
        a->has_mac = true;
        a->th = true;
    } else if (d->type == BT_DATA_SVC_DATA16 && d->data_len >= SB_SVC_MIN_LEN &&
               sys_get_le16(d->data) == SB_SVC_UUID && d->data[2] == SB_MODEL_OUTDOOR) {
        a->batt_pct = MIN(d->data[4] & 0x7F, 100);
        a->batt = true;
    }
    return true;
}

/* Caller holds lock. Disabled slots still match so SETUP shows a sensor's
 * values before the user switches it on. */
static int slot_of(const uint8_t mac[6])
{
    static const uint8_t zero_mac[6];

    for (int i = 0; i < EXT_SLOTS; i++) {
        if (memcmp(cfg.slot[i].mac, zero_mac, 6) && !memcmp(cfg.slot[i].mac, mac, 6)) {
            return i;
        }
    }
    return -1;
}

/* Caller holds lock */
static bool all_seen(void)
{
    const uint8_t full = EXT_SEEN_TH | EXT_SEEN_BATT;

    for (int i = 0; i < EXT_SLOTS; i++) {
        if (cfg.slot[i].en && (cache.r[i].seen & full) != full) {
            return false;
        }
    }
    return true;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
                         struct net_buf_simple *buf)
{
    struct sb_adv a = {0};
    uint8_t mac[6];
    bool wake = false;

    ARG_UNUSED(adv_type);

    bt_data_parse(buf, parse_ad, &a);
    if (!a.th && !a.batt) {
        return;
    }

    /* bt_addr_t is little-endian, the config holds the label's MSB-first
     * spelling. The address type is ignored on purpose. The battery packet
     * carries no MAC of its own, so the address is the primary key. */
    for (int i = 0; i < 6; i++) {
        mac[i] = addr->a.val[5 - i];
    }

    k_spinlock_key_t key = k_spin_lock(&lock);
    int slot = slot_of(mac);

    if (slot < 0 && a.has_mac) {
        slot = slot_of(a.mac);
    }
    if (slot >= 0) {
        ext_reading_t *r = &cache.r[slot];

        if (a.th) {
            r->temp_cc = a.temp_cc;
            r->hum = a.hum;
            r->last_ms = k_uptime_get();
            r->seen |= EXT_SEEN_TH;
        }
        if (a.batt) {
            r->batt = a.batt_pct;
            r->seen |= EXT_SEEN_BATT;
        }
        r->rssi = rssi;
        if (signal_all_seen && all_seen()) {
            signal_all_seen = false;
            wake = true;
        }
    }
    k_spin_unlock(&lock, key);

    if (wake) {
        k_sem_give(&all_seen_sem);
    }
}

int ext_scan_once(ext_data_t *out, uint32_t timeout_ms)
{
    const struct bt_le_scan_param param = {
        .type     = BT_LE_SCAN_TYPE_ACTIVE,
        .options  = BT_LE_SCAN_OPT_NONE,
        .interval = SCAN_INTERVAL,
        .window   = SCAN_WINDOW,
    };
    int64_t start = k_uptime_get();
    k_spinlock_key_t key;
    int err;

    memset(out, 0, sizeof(*out));

    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("bt_enable failed: %d", err);
        return err;
    }

    key = k_spin_lock(&lock);
    config_get_ext(&cfg);
    memset(&cache, 0, sizeof(cache));
    signal_all_seen = true;
    k_spin_unlock(&lock, key);
    k_sem_reset(&all_seen_sem);

    err = bt_le_scan_start(&param, device_found);
    if (err) {
        LOG_ERR("Scan start failed: %d", err);
    } else {
        /* ≤5 s slices so a long timeout can't starve the 60 s watchdog */
        for (uint32_t waited = 0; waited < timeout_ms; waited += 5000U) {
            if (k_sem_take(&all_seen_sem, K_MSEC(MIN(5000U, timeout_ms - waited))) == 0) {
                break;
            }
            watchdog_feed();
        }
        (void)bt_le_scan_stop();
    }

    key = k_spin_lock(&lock);
    signal_all_seen = false;
    *out = cache;
    k_spin_unlock(&lock, key);

    for (int i = 0; i < EXT_SLOTS; i++) {
        const ext_reading_t *r = &out->r[i];

        if (!cfg.slot[i].en) {
            continue;
        }
        if (r->seen & EXT_SEEN_TH) {
            LOG_INF("Ext slot %d: %s%d.%02d C %u%% batt %u%% rssi %d", i,
                    r->temp_cc < 0 ? "-" : "", abs(r->temp_cc) / 100, abs(r->temp_cc) % 100,
                    r->hum, r->batt, r->rssi);
        } else {
            LOG_WRN("Ext slot %d: not heard", i);
        }
    }
    LOG_INF("Ext scan done in %u ms", (uint32_t)(k_uptime_get() - start));

    /* Controller off before System OFF — a running radio/HFXO would wreck
     * the sleep current. */
    int derr = bt_disable();

    if (derr) {
        LOG_ERR("bt_disable failed: %d", derr);
    }
    return err;
}

int ext_scan_start(void)
{
    const struct bt_le_scan_param param = {
        .type     = BT_LE_SCAN_TYPE_ACTIVE,
        .options  = BT_LE_SCAN_OPT_NONE,
        .interval = SCAN_INTERVAL,
        .window   = SCAN_WINDOW,
    };

    ext_sensors_reload();

    int err = bt_le_scan_start(&param, device_found);

    if (err) {
        LOG_ERR("Live scan start failed: %d", err);
    } else {
        LOG_INF("Live scan started");
    }
    return err;
}

void ext_scan_stop(void)
{
    int err = bt_le_scan_stop();

    if (err && err != -EALREADY) {
        LOG_WRN("Scan stop failed: %d", err);
    }
}

void ext_sensors_reload(void)
{
    ext_config_t next;

    config_get_ext(&next);

    k_spinlock_key_t key = k_spin_lock(&lock);

    for (int i = 0; i < EXT_SLOTS; i++) {
        /* A reading stays only while its slot still names the same sensor */
        if (memcmp(cfg.slot[i].mac, next.slot[i].mac, 6)) {
            memset(&cache.r[i], 0, sizeof(cache.r[i]));
        }
    }
    cfg = next;
    k_spin_unlock(&lock, key);
}

void ext_get_cached(ext_data_t *out)
{
    int64_t now = k_uptime_get();

    k_spinlock_key_t key = k_spin_lock(&lock);
    *out = cache;
    k_spin_unlock(&lock, key);

    for (int i = 0; i < EXT_SLOTS; i++) {
        if ((out->r[i].seen & EXT_SEEN_TH) && now - out->r[i].last_ms > FRESH_MS) {
            out->r[i].seen &= ~EXT_SEEN_TH;
        }
    }
}
