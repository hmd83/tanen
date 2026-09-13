#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include "fsm.h"
#include "power.h"
#include "watchdog.h"
#include "../drivers/lora.h"
#include "../drivers/weight.h"
#include "../config/config.h"
#include "../features/measurement/measure.h"
#if IS_ENABLED(CONFIG_TANENBASE_TEMPCOMP)
#include "../features/measurement/tempcomp.h"
#endif
#include "../features/transmission/transmit.h"
#include "../features/ext_sensors/ext_sensors.h"
#include "../features/anomaly/anomaly.h"
#if IS_ENABLED(CONFIG_TANENBASE_BLE_CONFIG)
#include "../features/ble_config/ble_svc.h"
#endif

LOG_MODULE_REGISTER(fsm, LOG_LEVEL_INF);

#define LED0_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static fsm_state_t current_state;

/* k_sleep in ≤30s slices so long debug/fallback sleeps don't starve the
 * 60s watchdog (only System OFF stops the WDT). */
static void sleep_with_feed(uint32_t seconds)
{
    while (seconds > 0) {
        uint32_t slice = MIN(seconds, 30U);
        k_sleep(K_SECONDS(slice));
        watchdog_feed();
        seconds -= slice;
    }
}

int fsm_init(void)
{
    if (!gpio_is_ready_dt(&led)) {
        LOG_ERR("LED device not ready");
        return -ENODEV;
    }

    int ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure LED pin: %d", ret);
        return ret;
    }

    ret = config_init();
    if (ret < 0) {
        LOG_WRN("Config init failed: %d", ret);
    }

    return 0;
}

static void fsm_setup(void)
{
    current_state = FSM_STATE_SETUP;
    LOG_INF(">> State: SETUP (button/reset wake)");

    /* LED on to indicate setup mode */
    gpio_pin_set_dt(&led, 1);

    /* Init sensors for live diagnostic readings */
    measure_init();

#if IS_ENABLED(CONFIG_TANENBASE_BLE_CONFIG)
    /* BLE config service — blocks until done command or timeout */
    int err = ble_config_run(CONFIG_TANENBASE_SETUP_TIMEOUT);
    if (err) {
        LOG_WRN("BLE config exited: %d", err);
    }
#else
    /* No BLE — just wait setup timeout then continue */
    k_sleep(K_SECONDS(CONFIG_TANENBASE_SETUP_TIMEOUT));
#endif

    gpio_pin_set_dt(&led, 0);
    /* Power down NAU7802 analog — fsm_measure_and_decide re-inits */
    weight_sleep();
    LOG_INF("SETUP complete");
}

static void fsm_measure_and_decide(void)
{
    current_state = FSM_STATE_MEASUREMENT;
    LOG_INF(">> State: MEASUREMENT");

    /* SX1262 driver SYS_INIT calls Radio.Sleep(WarmStart=1) → ~600 nA after
     * boot. No explicit sleep needed here. */

    /* Init sensors and run measurement cycle */
    measure_init();
    measurement_data_t data;
    int err = measure_run(&data);
    if (err) {
        LOG_ERR("Measurement cycle failed: %d", err);
    }
    watchdog_feed();

    uint32_t meas_count;
    config_get_measurement_count(&meas_count);
    meas_count++;
    err = config_set_measurement_count(meas_count);
    if (err) {
        LOG_ERR("meas_count persist failed: %d", err);
    }

#if IS_ENABLED(CONFIG_TANENBASE_TEMPCOMP)
    /* Carry the thermal-lag filter across System OFF — once per cycle, not per
     * measure_run(), so the 2 s BLE live view can't hammer ZMS. */
    err = tempcomp_save();
    if (err) {
        LOG_ERR("tempcomp persist failed: %d", err);
    }
#endif

    /* Check if TX is needed (delta / heartbeat) */
    anomaly_result_t result = {0};
    anomaly_check(&data, &result);

    /* A previously failed TX (join fail / no radio) must not stay silent
     * until the next heartbeat modulo — retry on the very next wake. */
    uint8_t tx_pending = 0;
    config_get_tx_pending(&tx_pending);
    if (tx_pending && !result.should_tx) {
        result.should_tx = true;
        result.flags |= ANOMALY_FLAG_HEARTBEAT;
        LOG_INF("TX pending from failed cycle — retrying");
    }

    if (result.should_tx) {
        current_state = FSM_STATE_TRANSMISSION;
        LOG_INF(">> State: TRANSMISSION (flags=0x%02x)", result.flags);

        /* Extended Mode BLE sensors — scanned only on wakes that transmit,
         * and before lora_init() so the scan can't straddle the LoRaMAC
         * join/RX windows. A failed scan still sends the base frame. */
        const ext_data_t *ext_p = NULL;
#if IS_ENABLED(CONFIG_TANENBASE_EXT_SENSORS)
        ext_data_t ext;

        if (config_ext_active()) {
            (void)ext_scan_once(&ext, CONFIG_TANENBASE_EXT_SCAN_TIMEOUT_MS);
            ext_p = &ext;
            watchdog_feed();
        }
#endif

        /* Only init LoRa when TX is needed — saves power on measure-only wakes */
        err = lora_init();
        if (err) {
            LOG_ERR("LoRa init failed: %d — TX deferred to next wake", err);
            config_set_tx_pending(1);
        } else {
            watchdog_feed();
            err = transmit_run(&data, ext_p, result.flags);
            if (!err) {
                /* Update last-TX'd values on success — only fields that
                 * carried a valid reading (sentinel must not become the
                 * delta baseline). */
                if (data.valid & MEAS_VALID_WEIGHT) {
                    config_set_last_tx_weight(data.weight_g);
                }
                if (data.valid & MEAS_VALID_TEMP) {
                    config_set_last_tx_temp(data.temp_cc);
                }
                config_set_tx_pending(0);
            } else {
                config_set_tx_pending(1);
            }
            lora_session_save();
            /* sx12xx_common puts radio back in sleep on TX done */
        }
    } else {
        LOG_INF("No TX needed — measure only");
    }
    watchdog_feed();

    /* Enter System OFF */
    current_state = FSM_STATE_SLEEP;
    LOG_INF(">> State: SLEEP");

    gpio_pin_set_dt(&led, 0);

    /* Full NAU7802 power-down (PUD=0, ~1µA — preserves OCAL1 via ZMS) */
    weight_sleep();

    uint32_t ms_interval;
    config_get_ms_interval(&ms_interval);

    err = power_off(ms_interval);
    /* Only reached if power_off skipped or failed — sleep in watchdog-fed
     * slices (a plain k_sleep(180s) here would trip the 60s WDT). */
    if (err == -ENOTSUP) {
        LOG_INF("System OFF disabled — looping");
        sleep_with_feed(CONFIG_TANENBASE_DEBUG_LOOP_INTERVAL);
    } else {
        LOG_ERR("power_off failed: %d — fallback sleep %us", err, ms_interval);
        sleep_with_feed(ms_interval);
    }
}

void fsm_run(void)
{
    wake_reason_t reason = power_get_wake_reason();

    /* SETUP only on explicit button press. Reset/power-on goes straight to
     * measure to avoid 180s BLE advertise window on every fresh boot. */
    if (reason == WAKE_REASON_BUTTON) {
        fsm_setup();
    }

    fsm_measure_and_decide();
}

fsm_state_t fsm_get_state(void)
{
    return current_state;
}
