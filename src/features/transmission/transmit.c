#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include "transmit.h"
#include "../../drivers/lora.h"
#include "../anomaly/anomaly.h"

LOG_MODULE_REGISTER(transmit, LOG_LEVEL_INF);

#define PAYLOAD_LEN 8

/* Sentinels for failed sensors — server-side decoder maps these to null.
 * 0xFFFFFF g (16777.215 kg) sits far above the 999 kg design ceiling
 * (measure.c clamps at 999000), so a maxed-out real reading can never be
 * confused with a dead sensor. 0x7FFF cc (327 °C) is likewise outside any
 * physically plausible hive reading. */
#define SENTINEL_U24 0xFFFFFFu
#define SENTINEL_I16 0x7FFF

int transmit_run(const measurement_data_t *data, uint8_t flags)
{
    uint8_t payload[PAYLOAD_LEN];

    /* Bytes 0-2: weight in grams (uint24 BE, 0-999000 valid / 999 kg) */
    sys_put_be24((data->valid & MEAS_VALID_WEIGHT) ? data->weight_g
                                                   : SENTINEL_U24, &payload[0]);

    /* Bytes 3-4: temperature in centi-Celsius (int16 BE) */
    sys_put_be16((data->valid & MEAS_VALID_TEMP) ? (uint16_t)data->temp_cc
                                                 : SENTINEL_I16, &payload[3]);

    /* Bytes 5-6: battery in millivolts (uint16 BE) */
    sys_put_be16((data->valid & MEAS_VALID_BATTERY) ? data->battery_mv
                                                    : 0xFFFF, &payload[5]);

    /* Byte 7: anomaly flags */
    payload[7] = flags;

    LOG_INF("Payload: %02X %02X %02X %02X %02X %02X %02X %02X (%d bytes)",
            payload[0], payload[1], payload[2], payload[3],
            payload[4], payload[5], payload[6], payload[7], PAYLOAD_LEN);

    /* Confirmed uplink for weight anomaly, unconfirmed otherwise */
    bool confirmed = (flags & ANOMALY_FLAG_WEIGHT) != 0;

    int err = lora_send(payload, PAYLOAD_LEN, confirmed);
    if (err) {
        LOG_ERR("Uplink failed: %d", err);
        return err;
    }

    LOG_INF("Uplink sent (%s)", confirmed ? "confirmed" : "unconfirmed");
    return 0;
}
