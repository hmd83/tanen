#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include "transmit.h"
#include "../../config/config.h"
#include "../../drivers/lora.h"
#include "../anomaly/anomaly.h"

LOG_MODULE_REGISTER(transmit, LOG_LEVEL_INF);

#define PAYLOAD_BASE_LEN 8
#define PAYLOAD_MAX_LEN  (PAYLOAD_BASE_LEN + EXT_SLOTS * EXT_BLOCK_LEN)

/* Sentinels for failed sensors — server-side decoder maps these to null.
 * 0xFFFFFF g (16777.215 kg) sits far above the 999 kg design ceiling
 * (measure.c clamps at 999000), so a maxed-out real reading can never be
 * confused with a dead sensor. 0x7FFF cc (327 °C) is likewise outside any
 * physically plausible hive reading. */
#define SENTINEL_U24 0xFFFFFFu
#define SENTINEL_I16 0x7FFF

/* One 5-byte block per enabled BLE sensor heard this scan, in slot order.
 * A sensor that was not heard costs no airtime: it only sets
 * EXT_FLAG_MISSING. Returns the new payload length. */
static size_t append_ext(uint8_t *payload, size_t len, const ext_data_t *ext,
                         uint8_t *flags)
{
    ext_config_t cfg;

    config_get_ext(&cfg);
    if (!ext || !cfg.enabled) {
        return len;
    }

    for (int i = 0; i < EXT_SLOTS; i++) {
        const ext_reading_t *r = &ext->r[i];

        if (!cfg.slot[i].en) {
            continue;
        }
        if (!(r->seen & EXT_SEEN_TH)) {
            *flags |= EXT_FLAG_MISSING;
            continue;
        }
        payload[len] = (EXT_BLOCK_TYPE_SWITCHBOT_TH << 4) |
                       ((cfg.slot[i].role == EXT_ROLE_OUT) ? EXT_BLOCK_ROLE_OUT : 0) |
                       (uint8_t)i;
        sys_put_be16((uint16_t)r->temp_cc, &payload[len + 1]);
        payload[len + 3] = r->hum;
        payload[len + 4] = (r->seen & EXT_SEEN_BATT) ? r->batt : EXT_BATT_UNKNOWN;
        len += EXT_BLOCK_LEN;
    }
    return len;
}

int transmit_run(const measurement_data_t *data, const ext_data_t *ext, uint8_t flags)
{
    uint8_t payload[PAYLOAD_MAX_LEN];
    uint8_t out_flags = flags;

    /* Bytes 0-2: weight in grams (uint24 BE, 0-999000 valid / 999 kg) */
    sys_put_be24((data->valid & MEAS_VALID_WEIGHT) ? data->weight_g
                                                   : SENTINEL_U24, &payload[0]);

    /* Bytes 3-4: temperature in centi-Celsius (int16 BE) */
    sys_put_be16((data->valid & MEAS_VALID_TEMP) ? (uint16_t)data->temp_cc
                                                 : SENTINEL_I16, &payload[3]);

    /* Bytes 5-6: battery in millivolts (uint16 BE) */
    sys_put_be16((data->valid & MEAS_VALID_BATTERY) ? data->battery_mv
                                                    : 0xFFFF, &payload[5]);

    /* Bytes 8..: Extended Mode blocks */
    size_t len = append_ext(payload, PAYLOAD_BASE_LEN, ext, &out_flags);

    /* Byte 7: anomaly flags (+ EXT_FLAG_MISSING) */
    payload[7] = out_flags;

    LOG_HEXDUMP_INF(payload, len, "Payload:");

    /* Confirmed uplink for weight anomaly, unconfirmed otherwise */
    bool confirmed = (flags & ANOMALY_FLAG_WEIGHT) != 0;

    int err = lora_send(payload, len, confirmed);
    if (err) {
        LOG_ERR("Uplink failed: %d", err);
        return err;
    }

    LOG_INF("Uplink sent (%zu bytes, %s)", len, confirmed ? "confirmed" : "unconfirmed");
    return 0;
}
