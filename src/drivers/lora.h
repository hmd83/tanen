#ifndef TANENBASE_LORA_H
#define TANENBASE_LORA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

int lora_init(void);

/* Save LoRaWAN session to flash (call before System OFF) */
int lora_session_save(void);

/* Send uplink on port 1. confirmed=true for LORAWAN_MSG_CONFIRMED. */
int lora_send(const uint8_t *data, size_t len, bool confirmed);

#endif
