#include <stdbool.h>
#include <stdlib.h>
#include <zephyr/logging/log.h>
#include "measure.h"
#include "../../drivers/weight.h"
#include "../../drivers/temp.h"
#include "../../drivers/battery.h"
#include "../../config/config.h"
#if IS_ENABLED(CONFIG_TANENBASE_TEMPCOMP)
#include "tempcomp.h"
#endif

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

#if IS_ENABLED(CONFIG_TANENBASE_TEMPCOMP)
    tempcomp_init();
#endif

    return 0;
}

int measure_run(measurement_data_t *out)
{
    int failures = 0;
    measurement_data_t d = {0};

    LOG_INF("Cycle start");

    /* Weight: raw ADC → calibrated grams. Clamping is deferred to after the
     * temperature correction below, which needs the temp read first. */
    int32_t grams = 0;
    bool have_weight = false;
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
        grams = ((raw_weight - offset) * 1000) / factor;
        have_weight = true;
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

#if IS_ENABLED(CONFIG_TANENBASE_TEMPCOMP)
    /* Thermal drift correction — 16 g of phantom weight per kelvin on this
     * cell + mount, so this is worth ~6.5x on the residual. Needs a live temp
     * reading: with the probe dead the raw weight is reported uncorrected
     * rather than corrected against a stale t_eff. */
    if (d.valid & MEAS_VALID_TEMP) {
        int32_t corr_mg = tempcomp_correction_mg(temp_mc);

        if (have_weight) {
            grams += (corr_mg + (corr_mg >= 0 ? 500 : -500)) / 1000;
        }
    } else if (have_weight) {
        LOG_WRN("tempcomp: no temp reading — weight uncorrected");
    }
#endif

    if (have_weight) {
        /* 999 kg design ceiling — matches the wire format's 3-byte weight
         * field (max 999000, well clear of its 0xFFFFFF invalid-sensor
         * sentinel, see transmit.c). */
        d.weight_g = (grams < 0) ? 0 : (grams > 999000) ? 999000 : (uint32_t)grams;
        d.valid |= MEAS_VALID_WEIGHT;
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
