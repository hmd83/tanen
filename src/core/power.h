#ifndef TANENBASE_POWER_H
#define TANENBASE_POWER_H

#include <stdint.h>

typedef enum {
    WAKE_REASON_TIMER,   /* GRTC timed wake (normal cycle) */
    WAKE_REASON_BUTTON,  /* TanenButton P0.00 GPIO wake */
    WAKE_REASON_RESET,   /* Power-on reset or pin reset */
} wake_reason_t;

/**
 * Determine why the device woke up.
 * Reads GPIO latch register to distinguish button vs timer wake.
 * Must be called early in boot before GPIO latch is cleared.
 */
wake_reason_t power_get_wake_reason(void);

/**
 * Enter System OFF with GRTC timed wake.
 * Configures TanenButton (P0.00) as GPIO wake source.
 * Device reboots from scratch on wake — all RAM lost.
 *
 * @param sleep_seconds Seconds to sleep before wake.
 * @return negative errno on failure (never returns on success)
 */
int power_off(uint32_t sleep_seconds);

#endif
