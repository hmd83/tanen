#ifndef TANENBASE_MEASURE_H
#define TANENBASE_MEASURE_H

#include <stdint.h>
#include <zephyr/sys/util.h>

/* Per-field validity — a failed sensor must never masquerade as a real 0
 * (0 g would fire a false weight-anomaly confirmed uplink). */
#define MEAS_VALID_WEIGHT  BIT(0)
#define MEAS_VALID_TEMP    BIT(1)
#define MEAS_VALID_BATTERY BIT(2)

typedef struct {
    uint32_t weight_g;   /* calibrated weight in grams, 0-999000 (999 kg design ceiling) */
    int16_t  temp_cc;    /* temperature in centi-Celsius (0.01°C), signed both ways */
    uint16_t battery_mv; /* battery voltage in millivolts */
    uint8_t  valid;      /* MEAS_VALID_* bitmask */
} measurement_data_t;

/** Initialize all measurement sensors (weight, temp, battery) */
int measure_init(void);

/**
 * Run one measurement cycle: reads weight, temp, battery in sequence.
 * Applies calibration (zero_offset, scale_factor) to weight.
 * @param out  pointer to store results (may be NULL to discard)
 * @return 0 on success, negative errno if all 3 sensors failed
 */
int measure_run(measurement_data_t *out);

/** Return pointer to last measurement result */
const measurement_data_t *measure_last(void);

#endif
