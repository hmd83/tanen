/* test_app: measure_loop
 * Repeats MEASUREMENT phase every N seconds using System OFF (GRTC wake).
 * Mirrors fsm_measure_and_decide ordering so PPK2 trace matches production:
 *   measure_init -> measure_run -> weight_sleep -> power_off(N) -> reboot
 * No LoRa/BLE/anomaly. Works standalone — USB/UART optional. */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#if IS_ENABLED(CONFIG_LORA)
#include <radio.h>
#endif
#include "config/config.h"
#include "core/power.h"
#include "features/measurement/measure.h"
#include "drivers/weight.h"

LOG_MODULE_REGISTER(test_meas, LOG_LEVEL_INF);

int main(void)
{
    wake_reason_t reason = power_get_wake_reason();

    /* SWD recovery window — only on hard reset, skip on GRTC wake to keep
     * sleep-phase trace clean. */
    if (reason == WAKE_REASON_RESET) {
        k_sleep(K_SECONDS(5));
    }

    LOG_INF("=== test_app: measure_loop === (interval %d s)",
            CONFIG_TANENBASE_TEST_INTERVAL_S);

    int ret = config_init();
#if IS_ENABLED(CONFIG_LORA)
    /* 0=SLEEP 1=STDBY_RC 2=STDBY_XOSC 3=FS 4=TX 5=RX 6=RX_DC 7=CAD */
    LOG_INF("SX126x mode after SYS_INIT: %d", SX126xGetOperatingMode());
#endif
    if (ret < 0) {
        LOG_WRN("config_init: %d", ret);
    }

    /* Wake sensors (NAU7802 PUA on, DS18B20 cfg, ADC init) */
    ret = measure_init();
    if (ret < 0) {
        LOG_ERR("measure_init: %d", ret);
    }

    measurement_data_t d;
    ret = measure_run(&d);
    if (ret < 0) {
        LOG_ERR("measure_run: %d", ret);
    }

    /* Power down NAU7802 (PUD=0, ~1µA) before System OFF */
    weight_sleep();

    /* System OFF → GRTC wake → reboot back into main() */
    ret = power_off(CONFIG_TANENBASE_TEST_INTERVAL_S);
    LOG_ERR("power_off returned %d — falling back to k_sleep", ret);
    k_sleep(K_SECONDS(CONFIG_TANENBASE_TEST_INTERVAL_S));
    sys_reboot(SYS_REBOOT_COLD);
    return 0;
}
