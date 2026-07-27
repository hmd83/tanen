#ifndef TANENBASE_WEIGHT_H
#define TANENBASE_WEIGHT_H

#include <stdint.h>

/** Init NAU7802: full cold or warm bring-up + offset cal.
 *  Leaves analog (PUA) ON so subsequent reads are stable. */
int weight_init(void);

/** Read raw 24-bit ADC. Assumes weight_init was called and PUA is on. */
int weight_read(int32_t *raw);

/** Power down analog (PUA=0, keeps PUD=1 so regs persist for fast wake).
 *  Caller MUST invoke before deep sleep to avoid 1.5 mA drain. */
int weight_sleep(void);

#endif
