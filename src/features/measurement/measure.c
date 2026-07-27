#include <stdlib.h>
#include <zephyr/logging/log.h>
#include "measure.h"
#include "../../drivers/weight.h"
#include "../../drivers/temp.h"
#include "../../drivers/battery.h"
#include "../../config/config.h"

LOG_MODULE_REGISTER(measure, LOG_LEVEL_INF);

static measurement_data_t last_data;

int measure_init(void)
{
    int ret;

    ret = weight_init();
    if (ret < 0) {
        LOG_WRN("Weight init failed: %d — sensor offline", ret);
    }

    ret = temp_init();
    if (ret < 0) {
        LOG_WRN("Temp init failed: %d — sensor offline", ret);
    }

    ret = battery_init();
    if (ret < 0) {
        LOG_WRN("Battery init failed: %d — sensor offline", ret);
    }

    return 0;
}

int measure_run(measurement_data_t *out)
{
    int failures = 0;
    measurement_data_t d = {0};

    LOG_INF("Cycle start");

    /* Weight: raw ADC → calibrated grams */
    int32_t raw_weight;
    int err = weight_read(&raw_weight);
    if (err) {
        LOG_ERR("Weight read failed: %d", err);
        failures++;
    } else {
        int32_t offset, factor;
        config_get_zero_offset(&offset);
        config_get_scale_factor(&factor);
        if (factor == 0) {
            factor = 1000;
        }
        int32_t grams = ((raw_weight - offset) * 1000) / factor;
        /* 999 kg design ceiling — matches the wire format's 3-byte weight
         * field (max 999000, well clear of its 0xFFFFFF invalid-sensor
         * sentinel, see transmit.c). */
        d.weight_g = (grams < 0) ? 0 : (grams > 999000) ? 999000 : (uint32_t)grams;
        d.valid |= MEAS_VALID_WEIGHT;
    }

    /* Temperature: milli-Celsius → centi-Celsius */
    int32_t temp_mc;
    err = temp_read(&temp_mc);
    if (err) {
        LOG_ERR("Temp read failed: %d", err);
        failures++;
    } else {
        int32_t cc = temp_mc / 10;
        d.temp_cc = (cc < -32768) ? -32768 : (cc > 32767) ? 32767 : (int16_t)cc;
        d.valid |= MEAS_VALID_TEMP;
    }

    /* Battery: already in mV */
    int32_t batt_mv;
    err = battery_read(&batt_mv);
    if (err) {
        LOG_ERR("Battery read failed: %d", err);
        failures++;
    } else {
        d.battery_mv = (batt_mv < 0) ? 0 : (batt_mv > 65535) ? 65535 : (uint16_t)batt_mv;
        d.valid |= MEAS_VALID_BATTERY;
    }

    LOG_INF("Weight=%u g, Temp=%d.%02d C, Battery=%u mV (valid=0x%x)",
            d.weight_g,
            d.temp_cc / 100, abs(d.temp_cc % 100),
            d.battery_mv, d.valid);

    last_data = d;
    if (out) {
        *out = d;
    }

    return (failures == 3) ? -EIO : 0;
}

const measurement_data_t *measure_last(void)
{
    return &last_data;
}
