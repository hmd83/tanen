#ifndef TANENBASE_TRANSMIT_H
#define TANENBASE_TRANSMIT_H

#include <stdint.h>
#include <zephyr/sys/util.h>
#include "../measurement/measure.h"
#include "../ext_sensors/ext_sensors.h"

/* Extended Mode block, appended after byte 7 once per BLE sensor heard */
#define EXT_BLOCK_LEN                5
#define EXT_BLOCK_TYPE_SWITCHBOT_TH  1       /* desc bits 7-4 */
#define EXT_BLOCK_ROLE_OUT           BIT(2)  /* desc bit 2; bits 1-0 = slot */
#define EXT_BATT_UNKNOWN             0xFF

/* Flags byte bit 3 — anomaly.h owns bits 0-2 */
#define EXT_FLAG_MISSING             BIT(3)

/**
 * Build big-endian payload and send LoRaWAN uplink.
 *
 * Payload format (8 bytes + 5 per BLE sensor heard, max 23):
 *   Bytes 0-2 (uint24 BE): weight in grams, 0-999000 (999 kg design ceiling).
 *                          0xFFFFFF = weight sensor invalid.
 *   Bytes 3-4 (int16 BE):  temperature in centi-Celsius (0.01°C), signed.
 *                          0x7FFF = temp sensor invalid.
 *   Bytes 5-6 (uint16 BE): battery in millivolts. 0xFFFF = battery invalid.
 *   Byte  7   (uint8):     anomaly flags; bit 3 = an enabled BLE sensor was
 *                          not heard (its block is omitted)
 *   Per BLE sensor heard, in slot order:
 *     Byte  0   (uint8):    desc — bits 7-4 type (1 = SwitchBot T/H),
 *                           bit 2 role (1 = outside), bits 1-0 slot
 *     Bytes 1-2 (int16 BE): temperature in centi-Celsius
 *     Byte  3   (uint8):    humidity %RH
 *     Byte  4   (uint8):    battery %, 0xFF = unknown
 *
 * Uses confirmed uplink if ANOMALY_FLAG_WEIGHT is set.
 *
 * @param data  Calibrated measurement results
 * @param ext   BLE sensor readings, NULL = base frame only
 * @param flags Anomaly flags bitmask
 * @return 0 on success, negative errno on failure
 */
int transmit_run(const measurement_data_t *data, const ext_data_t *ext, uint8_t flags);

#endif
