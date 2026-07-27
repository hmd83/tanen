# TanenBase — Open Questions

## Resolved (Default Assumptions)

| # | Question | Default Assumption |
|---|----------|-------------------|
| 1 | LoRaWAN payload format details (port, field sizes) | Per TRD Section 6: big-endian, port 1, fields: weight(4B), temp(2B), battery(2B), flags(1B) |
| 2 | T_TX default interval | 15 minutes (configurable via BLE/NVS) |
| 3 | T_MEAS default interval | 14 minutes (must be < T_TX, enforced by firmware) |
| 4 | Anomaly weight delta threshold | 500g default (configurable via BLE/NVS) |
| 5 | BLE advertising name | "TanenBase" |
| 6 | BLE advertising timeout in Setup mode | 5 minutes, then enter sleep |
| 7 | NAU7802 gain setting | 128x (default for load cell) |
| 8 | NAU7802 sample rate | 10 SPS (lowest power) |
| 9 | DS18B20 resolution | 12-bit (default) |
| 10 | Max LoRaWAN join retries before sleep | 8 attempts with exponential backoff (Zephyr built-in) |
| 11 | Watchdog timeout | 120 seconds (production only) |
| 12 | NVS partition size | 4KB |

## Unresolved

| # | Question | Impact | Proposed Default |
|---|----------|--------|-----------------|
| 1 | How does user enter Setup mode? Button press on P0.00? Long press duration? | FSM entry logic | 3-second long press on TanenButton (P0.00) enters Setup |
| 2 | Should downlink commands be supported? (remote config change via TTN) | Transmission feature scope | Implemented — FPort: 10=tx_interval, 11=ms_interval, 12=weight_th, 13=temp_th |
| 3 | LoRaWAN confirmed vs unconfirmed uplinks? | Transmission reliability vs airtime | Unconfirmed by default, configurable |
| 4 | Multiple DS18B20 sensors on same 1-Wire bus? | Devicetree + measurement feature | Single sensor for MVP, extensible |
| 5 | OTA firmware update mechanism? | Long-term maintenance | Out of scope for MVP |
| 6 | Web app (index.html) — who builds it? Scope? | Sprint 10 | Basic config page: set intervals, calibrate, view readings, set LoRaWAN keys |
| 7 | ~~TTN payload decoder — update for failed-sensor sentinels → null?~~ — **RESOLVED (2026-07-07):** done as part of Sprint 14's payload widening. `ttndecoder/beepdecoder.js` + `custumdecoder.js` now map `0xFFFFFF`/`0xFFFF`/`0x7FFF` to `null`. | Server-side decoder correctness | — |
| 8 | Adopt MCUboot/sysbuild builds now, or defer to a hardware rev? | One-time reprovision needed for any already-flashed units (ZMS location pin) | Adopt now — `pm_static.yml` protects existing storage_partition address |
| 9 | Production MCUboot signing key — where does it live / who holds it? | `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` currently unset (dev default) | Generate + store outside repo, same policy as `prj_credentials.conf` |
| 10 | ~~Web Bluetooth pairing UX for encrypted cred writes~~ — **RESOLVED (2026-07-07): reverted.** `BT_GATT_PERM_WRITE_ENCRYPT` on AppKey/DevEUI/JoinEUI broke real provisioning — Web Bluetooth has no `pair()` API, the OS-triggered SMP pairing hung mid-write, link died on supervision timeout (disconnect reason 8), `bt_le_adv_start` then failed `-ENOMEM`. Now plain R/W. | Credential confidentiality relies on physical proximity (BLE range) only, not link encryption | If encryption is required later: a companion native/mobile app that can call `pair()`, or app-layer encryption of the AppKey payload before the GATT write |
| 11 | ~~BEEP not receiving uplinks after payload widened to 8 bytes~~ — **RESOLVED (2026-07-07):** not a payload issue. BEEP's `/api/lora_sensors` resolves the target device via `payload_fields.hardware_serial` (TTN Stack V2) when no explicit `key` is supplied; this app's webhook is Stack V3, which puts the identifier at `end_device_ids.dev_eui` instead, so BEEP silently failed to route the (correctly-decoded) measurement to any device. Fixed by setting the TTN webhook base URL to `https://api.beep.nl/api/lora_sensors?key=<DevEUI>`. See Sprint 14 in PLAN.md and `ttndecoder/Beep-Sensor-data-API-v0.5.pdf`. | External webhook config only — no firmware/decoder involvement | Document the `?key=<DevEUI>` webhook URL requirement wherever TTN integration setup is documented, so a future BEEP webhook (re)creation doesn't regress this |
