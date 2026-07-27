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
 *   - Button/Reset → SETUP (BLE + sensors, blocks up to timeout)
 *   - Timer → MEASUREMENT → anomaly check → TX if needed → SLEEP
 * Never returns on success (ends in System OFF).
 */
void fsm_run(void);

/** Get current state (for BLE status reporting) */
fsm_state_t fsm_get_state(void);

#endif
