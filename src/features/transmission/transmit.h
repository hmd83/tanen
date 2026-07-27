#ifndef TANENBASE_TRANSMIT_H
#define TANENBASE_TRANSMIT_H

#include <stdint.h>
#include "../measurement/measure.h"

/**
 * Build 8-byte big-endian payload and send LoRaWAN uplink.
 *
 * Payload format:
 *   Bytes 0-2 (uint24 BE): weight in grams, 0-999000 (999 kg design ceiling).
 *                          0xFFFFFF = weight sensor invalid.
 *   Bytes 3-4 (int16 BE):  temperature in centi-Celsius (0.01°C), signed.
 *                          0x7FFF = temp sensor invalid.
 *   Bytes 5-6 (uint16 BE): battery in millivolts. 0xFFFF = battery invalid.
 *   Byte  7   (uint8):     anomaly flags
 *
 * Uses confirmed uplink if ANOMALY_FLAG_WEIGHT is set.
 *
 * @param data  Calibrated measurement results
 * @param flags Anomaly flags bitmask
 * @return 0 on success, negative errno on failure
 */
int transmit_run(const measurement_data_t *data, uint8_t flags);

#endif
