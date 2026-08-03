#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "core/fsm.h"
#include "core/power.h"
#include "core/watchdog.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
    /* Watchdog FIRST — everything after this is supervised (TRD 13.1) */
    int ret = watchdog_init();
    if (ret < 0) {
        LOG_ERR("Watchdog init failed: %d — continuing UNSUPERVISED", ret);
    }

    /* Flash safety window — only on hard reset so System OFF can't brick board.
     * Timer/button wakes skip this to save 5s per cycle. */
    wake_reason_t reason = power_get_wake_reason();
    if (reason == WAKE_REASON_RESET) {
        k_sleep(K_SECONDS(5));
    }
    LOG_INF("TanenBase v0.3.0 initialized");
    /* Zena (2026-08-03): simple demo edit — visible from VS Code via git pull */

    ret = fsm_init();
    if (ret < 0) {
        LOG_ERR("FSM init failed: %d", ret);
        /* Field device: a dead-on-init unit must not park in main's return
         * path — reboot via watchdog starvation is pointless; sleep-retry
         * instead. GRTC wake re-runs full init from ROM. */
        (void)power_off(CONFIG_TANENBASE_DEFAULT_MS_INTERVAL);
        return ret;
    }

    while (1) {
        watchdog_feed();
        fsm_run();
    }

    return 0;
}
