#ifndef TANENBASE_ANOMALY_H
#define TANENBASE_ANOMALY_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/sys/util.h>
#include "../measurement/measure.h"

#define ANOMALY_FLAG_WEIGHT    BIT(0)
#define ANOMALY_FLAG_TEMP      BIT(1)
#define ANOMALY_FLAG_HEARTBEAT BIT(2)

typedef struct {
    bool    should_tx;
    uint8_t flags;
} anomaly_result_t;

/**
 * Check if current measurement warrants a TX.
 * Compares against last-TX'd values (from ZMS) and thresholds.
 * Checks heartbeat cycle counter.
 *
 * @param data   Current measurement
 * @param result Output: should_tx decision + flags bitmask
 * @return 0 on success
 */
int anomaly_check(const measurement_data_t *data, anomaly_result_t *result);

#endif
