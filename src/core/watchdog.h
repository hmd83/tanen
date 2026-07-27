#ifndef TANENBASE_WATCHDOG_H
#define TANENBASE_WATCHDOG_H

/**
 * Arm the task watchdog (software) backed by WDT31 (hardware fallback).
 * Timeout 60 s per TRD 13.1 / AC-10. Call FIRST in main, before any
 * blocking init. HW WDT stops in System OFF (wake = full reset re-arms).
 *
 * @return 0 on success, negative errno (device stays sw-only on hw failure)
 */
int watchdog_init(void);

/**
 * Feed the main-flow watchdog channel. Call at every FSM boundary and
 * inside any wait loop that can exceed ~30 s. Safe before init (no-op).
 */
void watchdog_feed(void);

#endif
