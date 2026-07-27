/* Two-stage watchdog:
 *  - task_wdt (software, k_timer driven from ISR) supervises the main FSM
 *    flow — catches a main thread stuck on a dead semaphore (e.g. LoRaMAC
 *    callback never fires because SX1262 BUSY is wedged, sysWQ deadlock).
 *  - WDT31 (hardware fallback, fed only by task_wdt itself) — catches the
 *    kernel/timer subsystem itself dying, hard faults with IRQs off, etc.
 * Channel timeout 60 s = TRD 13.1 / AC-10. All legitimate blocking ops
 * (join ≈13 s worst-case SF12, confirmed TX ≈13 s, cold sensor init ≈2 s)
 * fit inside one window; longer waits (SETUP mode) feed in slices. */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/logging/log.h>

#include "watchdog.h"

LOG_MODULE_REGISTER(watchdog, LOG_LEVEL_INF);

#define WDT_TIMEOUT_MS (60 * 1000)

static int wdt_channel = -1;

int watchdog_init(void)
{
	const struct device *hw_wdt = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(wdt31));

	if (hw_wdt != NULL && !device_is_ready(hw_wdt)) {
		LOG_ERR("WDT31 not ready — task watchdog runs sw-only");
		hw_wdt = NULL;
	}

	int ret = task_wdt_init(hw_wdt);
	if (ret) {
		LOG_ERR("task_wdt_init: %d", ret);
		return ret;
	}

	/* NULL callback = task_wdt reboots the SoC on expiry */
	ret = task_wdt_add(WDT_TIMEOUT_MS, NULL, NULL);
	if (ret < 0) {
		LOG_ERR("task_wdt_add: %d", ret);
		return ret;
	}
	wdt_channel = ret;

	LOG_INF("Watchdog armed: %d ms, hw fallback %s",
		WDT_TIMEOUT_MS, hw_wdt ? "WDT31" : "NONE");
	return 0;
}

void watchdog_feed(void)
{
	if (wdt_channel >= 0) {
		(void)task_wdt_feed(wdt_channel);
	}
}
