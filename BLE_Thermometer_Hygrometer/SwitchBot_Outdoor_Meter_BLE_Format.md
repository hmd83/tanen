# SwitchBot Outdoor Meter (WoIOSensorTH) BLE Format

This document explains how to decode the Bluetooth Low Energy (BLE) advertisement packets sent by the SwitchBot Outdoor Thermometer/Hygrometer. You can use this logic to implement a scanner on the nRF52 using the nRF Connect SDK or nRF5 SDK.

## Overview

The SwitchBot Outdoor Meter broadcasts two important AD (Advertisement Data) structures:
1. **Manufacturer Specific Data (`0xFF`)**: Contains the Temperature and Humidity.
2. **Service Data - 16-bit UUID (`0x16`)**: Contains the Battery level.

---

## 1. Manufacturer Specific Data (Temperature & Humidity)

When you receive a BLE advertisement packet, look for the AD type `0xFF` (Manufacturer Specific Data). The total length of the payload for the Outdoor Meter is typically 14 bytes.

**Example Payload (14 bytes):**
`69 09 eb 6b 04 06 0b 6b 3c 02 08 99 2c 00`

### Byte Mapping (0-indexed)

| Byte Index | Hex Value | Description |
|:---:|:---:|---|
| `0-1` | `69 09` | **Company Identifier**: `0x0969` (Woan Technology). Sent little-endian. |
| `2-7` | `eb 6b 04 06 0b 6b` | **MAC Address**: The BLE MAC address of the sensor. |
| `8-9` | `3c 02` | Status flags / Unknown. |
| `10` | `08` | **Temperature (Fractional)** |
| `11` | `99` | **Temperature (Integer & Sign)** |
| `12` | `2c` | **Humidity** |
| `13` | `00` | Unknown / Padding |

### C Code Example for Temperature & Humidity (nRF52)

```c
#include <stdint.h>
#include <stdbool.h>

// Assuming 'mfr_data' is a pointer to the 14-byte payload starting at byte 0 (0x69)
void decode_switchbot_mfr_data(const uint8_t* mfr_data, uint8_t length) {
    if (length < 13) return; // Ensure payload is long enough
    
    // Check Company ID (0x0969)
    if (mfr_data[0] != 0x69 || mfr_data[1] != 0x09) return;

    // 1. Decode Temperature
    uint8_t fraction_byte = mfr_data[10];
    uint8_t integer_byte = mfr_data[11];
    
    uint8_t fraction = fraction_byte & 0x0F;
    uint8_t integer = integer_byte & 0x7F;
    bool is_positive = (integer_byte & 0x80) != 0; // MSB (Bit 7) determines sign
    
    float temperature = integer + (fraction / 10.0f);
    if (!is_positive) {
        temperature = -temperature;
    }

    // 2. Decode Humidity
    uint8_t humidity_byte = mfr_data[12];
    uint8_t humidity = humidity_byte & 0x7F;
    
    // Print results
    // printf("Temp: %.1f C, Humidity: %d %%\n", temperature, humidity);
}
```

---

## 2. Service Data (Battery Level)

The device also broadcasts its battery level in the Service Data AD type (`0x16`) under the 16-bit UUID `0xFD3D`.

**Example Payload (5 bytes total, 3 bytes data):**
`3d fd 77 00 64`
*(Note: `3d fd` is the UUID `0xFD3D` in little-endian. The actual data is `77 00 64`)*

### Byte Mapping (Data Only, 0-indexed)

| Byte Index | Hex Value | Description |
|:---:|:---:|---|
| `0` | `77` | **Device Model**: `0x77` indicates the Outdoor Meter (`0x54` is the standard indoor meter). |
| `1` | `00` | Status flags / Unknown. |
| `2` | `64` | **Battery Level**: `0x64` = 100% |

### C Code Example for Battery (nRF52)

```c
// Assuming 'svc_data' points to the data AFTER the 16-bit UUID (i.e., starts at 0x77)
void decode_switchbot_svc_data(const uint8_t* svc_data, uint8_t length) {
    if (length < 3) return;
    
    // Check if it's the Outdoor Meter model (0x77)
    if (svc_data[0] == 0x77) {
        uint8_t battery = svc_data[2] & 0x7F;
        // printf("Battery: %d %%\n", battery);
    }
}
```

## Tips for nRF52 Implementation
- Depending on the SDK (nRF5 SDK vs nRF Connect SDK / Zephyr), the BLE stack might parse the AD structures for you and hand you just the data payload, or you may need to iterate through the raw advertisement packet looking for lengths and AD Types (`0xFF` and `0x16`).
- Because these devices broadcast frequently, you might receive the `0xFF` packet and the `0x16` packet in alternating broadcasts. Cache the battery value when you see the `0x16` packet, and cache the temperature when you see the `0xFF` packet.
