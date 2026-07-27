#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include "anomaly.h"
#include "../../config/config.h"

LOG_MODULE_REGISTER(anomaly, LOG_LEVEL_INF);

int anomaly_check(const measurement_data_t *data, anomaly_result_t *result)
{
    result->should_tx = false;
    result->flags = 0;

    uint32_t last_weight;
    uint16_t thresh_w;
    int16_t last_temp;
    uint16_t thresh_t;
    uint32_t tx_interval, ms_interval, meas_count;

    config_get_last_tx_weight(&last_weight);
    config_get_last_tx_temp(&last_temp);
    config_get_anomaly_weight_threshold(&thresh_w);
    config_get_anomaly_temp_threshold(&thresh_t);
    config_get_tx_interval(&tx_interval);
    config_get_ms_interval(&ms_interval);
    config_get_measurement_count(&meas_count);

    /* Weight delta — int32_t cast prevents unsigned underflow. Skipped when
     * the reading is invalid: a dead NAU7802 reads as 0 g and would fire a
     * false swarm/theft confirmed uplink every cycle. */
    if (data->valid & MEAS_VALID_WEIGHT) {
        int32_t w_diff = abs((int32_t)data->weight_g - (int32_t)last_weight);
        if (w_diff >= (int32_t)thresh_w) {
            result->flags |= ANOMALY_FLAG_WEIGHT;
            result->should_tx = true;
            LOG_INF("Weight delta %d g (thresh %u)", w_diff, thresh_w);
        }
    } else {
        LOG_WRN("Weight invalid — delta check skipped");
    }

    /* Temp delta — int32_t cast prevents signed overflow */
    if (data->valid & MEAS_VALID_TEMP) {
        int32_t t_diff = abs((int32_t)data->temp_cc - (int32_t)last_temp);
        if (t_diff >= (int32_t)thresh_t) {
            result->flags |= ANOMALY_FLAG_TEMP;
            result->should_tx = true;
            LOG_INF("Temp delta %d cc (thresh %u)", t_diff, thresh_t);
        }
    } else {
        LOG_WRN("Temp invalid — delta check skipped");
    }

    /* Heartbeat — division-by-zero guard */
    if (ms_interval == 0) {
        ms_interval = CONFIG_TANENBASE_DEFAULT_MS_INTERVAL;
    }
    uint32_t heartbeat_cycles = tx_interval / ms_interval;
    if (heartbeat_cycles == 0) {
        heartbeat_cycles = 1;
    }
    if ((meas_count % heartbeat_cycles) == 0) {
        result->flags |= ANOMALY_FLAG_HEARTBEAT;
        result->should_tx = true;
        LOG_INF("Heartbeat (cycle %u, every %u)", meas_count, heartbeat_cycles);
    }

    LOG_INF("Decision: tx=%d flags=0x%02x", result->should_tx, result->flags);
    return 0;
}
