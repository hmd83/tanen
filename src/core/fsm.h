#ifndef TANENBASE_FSM_H
#define TANENBASE_FSM_H

typedef enum {
    FSM_STATE_SETUP,
    FSM_STATE_MEASUREMENT,
    FSM_STATE_TRANSMISSION,
    FSM_STATE_SLEEP,
} fsm_state_t;

/** Initialize FSM (LED, config). Call once from main. */
int fsm_init(void);

/**
 * Run single-pass FSM. Checks wake reason:
 *   - Button → SETUP (BLE + sensors, blocks up to timeout) → MEASUREMENT
 *   - Timer → MEASUREMENT → anomaly check → TX if needed → SLEEP
 *   - Reset → MEASUREMENT. NOT setup: power-on, pin reset, watchdog and
 *     fatal-error reboots would otherwise each open a 180 s BLE advertise
 *     window, and a boot loop would sit in it. A unit that has just been
 *     flashed therefore does NOT come up in SETUP — press the button once
 *     it is asleep.
 * Never returns on success (ends in System OFF).
 */
void fsm_run(void);

/** Get current state (for BLE status reporting) */
fsm_state_t fsm_get_state(void);

#endif
