#ifndef TANENBASE_LORA_H
#define TANENBASE_LORA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

int lora_init(void);

/* Save LoRaWAN session to flash (call before System OFF) */
int lora_session_save(void);

/* Send uplink on port 1. confirmed=true for LORAWAN_MSG_CONFIRMED.
 * Returns LORA_TX_SKIPPED (positive) when a routine uplink was dropped for
 * TTN fair use at SF12 — not a failure, nothing to retry. */
#define LORA_TX_SKIPPED  1
int lora_send(const uint8_t *data, size_t len, bool confirmed);

/* Installer-initiated (BLE LoRa test): bypass the SF12 join backoff and the
 * fair-use skip for this wake. RAM-only, cleared by System OFF. */
void lora_set_manual(bool on);

#endif
