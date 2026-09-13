# Changelog

Notable changes to TanenBase. Dates are the date the work landed.

The format loosely follows [Keep a Changelog](https://keepachangelog.com/).
Detailed per-sprint history lives in [`docs/PLAN.md`](docs/PLAN.md).

## [Unreleased]

### Changed
- **Power budget: TX with Extended Mode measured at 139 mC** (2026-09-13, PPK2:
  11.13 s, 12.49 mA avg, 77.78 mA peak) — the BLE scan adds 76 mC to the 63 mC
  uplink. Lifetimes recomputed with BLE sensors: 4.6 → **3.3 y** at 15 min / 2 h,
  10 → 7 y at 60 min / 4 h; nodes without them are unchanged. POWER_BUDGET,
  README, ARCHITECTURE, TRD, user manual and landing page updated
- **Wording** (2026-09-13): the German setup page and manual say *Sensor(en)*
  instead of *Fühler* for the Bluetooth sensors; the landing page says *keine
  Cloud* instead of *keine Zwangs-Wolke* (English and Arabic aligned)
- **Cell identified and the voltage deviation re-assessed against the chemistry
  handbook** (2026-09-10). The cells are **BEVIGOR AAA Li/FeS₂**. Energizer's
  LiFeS₂ handbook corrects three things: the old "OCV is highest when cold" note
  was backwards (the low-drain plateau *rises* with temperature — a hot hive is
  the worst case); at µA drain Li/FeS₂ holds ~1.79 V/cell "nearly independent of
  depth of discharge", so the pack is above the nPM1300's 4.45 V recommended
  maximum for **essentially its whole life**, not only while fresh; and the
  1.79–1.83 V fresh spread puts a worst-case pack at **5.49 V against the 5.5 V
  absolute maximum**. Mitigations added (reject cells > ~1.80 V at assembly,
  pre-drain, keep the enclosure out of the sun); invented AAA pulse ratings
  replaced by the handbook's AA figures
- **The built manual PDF is no longer versioned.** `docs/BENUTZERHANDBUCH.pdf`
  (2.4 MB, rewritten on every text change) is out of the repo and gitignored;
  `scripts/build_manual_pdf.py` now builds HTML only and takes `--pdf` when a
  PDF is actually wanted. The HTML manual stays versioned (2026-09-10)
- **Battery pack: single ER14505 AA Li-SOCl₂ → 3× AAA 1.5 V Li/FeS₂ in series**
  (1200 mAh, 4.5 V nominal, **5.2 V measured fresh**). Life at 15 min / 2 h drops
  9.3 y → **4.6 y** (~10 y at 60 min / 4 h). Gains: no passivation and no
  depassivation step, −40 °C rating (closes the old cold-temperature question),
  and ~10× pulse margin instead of the ER14505's marginal 91 mA row. Cost: the
  pack runs **above the nPM1300's 4.45 V recommended VBAT maximum** until it
  falls below ~1.48 V/cell, with 0.3 V to the 5.5 V absolute maximum — an
  accepted, documented deviation (`docs/POWER_BUDGET.md` §5.2), not a fix.
  Firmware is unchanged; charge/current figures are still 3.6 V PPK2 captures and
  have not been re-measured at 4.5 V. Docs updated across README, POWER_BUDGET
  (§5/§5.2/§6/§7/§8/§9/§10), ARCHITECTURE, TRD deviations, CONTRIBUTING, the user
  manual and the site (2026-09-10)

### Fixed
- **QR scanner could not read the sensor labels** (2026-09-13). The printed code
  is ~5 mm; the camera opened at default resolution and scanned the whole frame,
  and a failing `BarcodeDetector` was swallowed so the scan looped silently. Now:
  1920×1080 with continuous focus and 2× zoom where supported, an aim square
  whose centre is scanned enlarged (alternating with the full frame), fallback to
  jsQR when `BarcodeDetector` errors, a "Foto aufnehmen" path through the phone's
  camera app with a tiled search, and an info line (reader, resolution, zoom).
  Verified on a phone. Labels should still print the code at ≥ 12–15 mm with a
  white margin
- **SETUP with a dead or absent load cell: web app dropped after ~5 s, node
  watchdog-reset** (2026-09-13). BLE live readings (and tare/calibrate) ran
  `measure_run()` on the cooperative system workqueue; gpio-I2C busy-spins, and
  with no NAU7802 every SCL edge spins out the 100 ms clock-stretch timeout
  (~3.5 s per read, re-queued every 2 s). That starved the BT RX thread and
  `main` → GATT stalled, then the 60 s watchdog reset the node, whose reset wake
  skips SETUP. BLE sensor I/O now runs on a dedicated preemptible work queue,
  a live read is skipped while the last one is pending, and the queue is drained
  before SETUP returns. Pre-dates Extended Mode; found testing it on a bare board

### Added
- **Extended Mode showcased on the landing page and in the README** (2026-09-13):
  own section with the sensor photo and the live setup-page screenshot
  (`media/BLE_Thermometer_Hygrometer_sensor.jpeg`,
  `media/screen_shots/BLE_Thermometer_Hygrometer_screen.jpeg`; 460 px copies in
  `site/media/`), nav link and spec-table row in DE/EN/AR
- **Extended Mode — up to 3 SwitchBot Outdoor Meter BLE sensors** (2026-09-13).
  At most 2 in-hive and 1 outside, set up in a new *Erweitert* tab of the web
  page (MAC typed or read from a QR label, role, per-sensor and master switch),
  stored in ZMS id 20; live readings show in the tab and on the overview.
  Scanned only on transmitting wakes and before the LoRa join (≤ 8 s, ends as
  soon as every enabled sensor has been heard); measure-only wakes never start
  BT. Each sensor heard appends a 5-byte block (slot/role, temperature,
  humidity, battery %) → 8/13/18/23-byte frames; a sensor not heard costs no
  airtime, only flags bit 3. `tanen-decoder.js` counts blocks from the length:
  the outside sensor becomes `t`/`TempOut` (+ `h`/`FeuchteOut`) and the DS18B20
  moves to `t_0`; in-hive sensors map to `t_i`/`h_i`/`TempIn`/`FeuchteIn` and
  `t_1`/`TempIn2`/`FeuchteIn2`. 8-byte frames decode exactly as before. New GATT
  characteristics `0x000E` ExtConfig and `0x000F` ExtLive; jsQR 1.4.0 vendored as
  `web/jsqr.js`, loaded only where the browser lacks `BarcodeDetector`.
  Verified on hardware (2026-09-13): three sensors live in SETUP, blocks in the
  uplink, TX wake measured at 139 mC
- **`ttndecoder/tanen-decoder.js`** — one uplink formatter for both supported
  platforms. Parses the 8-byte frame once and emits BEEP keys
  (`weight_kg`/`t`/`bv`) and beelogger keys (`Gewicht`/`TempOut`/`VBatt`) in the
  same `decoded_payload`; each server keeps what it knows. Invalid sensors stay
  `null` on both sides. The per-platform formatters (`beepdecoder.js`,
  `custumdecoder.js`, `tanen-beelogger-decoder.js`) are kept for reference
  (2026-09-09)
- beelogger documented as a supported platform alongside BEEP: README TTN
  section with the key-mapping table and the community-server webhook URL
  (`community.beelogger.de/<user>/{/devID}/beelogger_log.php?Passwort=…&LORA=1`
  — one webhook for the whole application, TTN device IDs named `beeloggerN`),
  `docs/ARCHITECTURE.md` "Server-Side Decoding", user manual chapter 6 rewritten
  from "BEEP-Plattform" to "Auswerteplattform" with a beelogger section, and a
  platform bullet on the site (de/en/ar). Both platforms confirmed receiving
  from the same uplink (2026-09-09)
- Open-source release: Apache-2.0 `LICENSE` + `NOTICE`, `README.md`,
  `CONTRIBUTING.md`, this changelog (2026-07-27)
- `prj_credentials.conf.example` so a fresh clone builds without secrets
- `scripts/ncs-env.ps1` — SDK/toolchain auto-discovery, overridable via
  `$env:NCS_ROOT` / `NCS_VERSION` / `NCS_TOOLCHAIN`
- `Tanen_Base_pcb/README.md` documenting XIAO footprint compatibility and
  fabrication notes
- **Second TanenButton on D19 (P0.00)** — a XIAO bottom pad, wired in parallel
  with the on-board P0.09 key. `button_arm_sense()` arms both as System OFF wake
  sources and `power_get_wake_reason()` ORs the two latches, so either press
  wakes into SETUP; they are read separately only so the log says which one
  fired. Carrier v0.1 does not route D19 — flying wire until v0.2 (2026-08-29)
- **`web/encoder.html`** — standalone LoRaWAN downlink builder for FPort 10–13
  (TxInterval, MsInterval, WeightThresh, TempThresh). Emits fPort + hex +
  base64 for the TTN console and a ready `down/push` body for the TTN API. No
  Bluetooth, so it works without a node in range
- Mandatory Wio-SX1262 rework documented in `Tanen_Base_pcb/README.md`: the
  kit's **K1 button + 10 kΩ pull-up sit on D0**, the DS18B20 1-Wire net, and
  with them fitted 1-Wire does not read at all. One trace cut lifts both off D0

### Fixed
- **nRF54LM20A anomaly [37]** — System OFF current above spec when entered soon
  after a pin reset or power cycle. Nordic's workaround (write 1 to the
  undocumented POWER register `0x5005340C`, ≥ 40 CPU cycles before System OFF)
  is applied in `power_off()`; the GRTC prepare that follows covers the delay
- **BLE "Send radio test" (ConfigCmd 0x04) sent a 1-byte `{0xFF}` uplink.** Both
  TTN decoders bail on `bytes.length < 8`, so the frame decoded to nothing and
  BEEP fell back to the raw byte — the phantom `255` seen on the platform. The
  test now sends the same 8-byte frame as a real uplink via `transmit_run()`
  (flags = 0, unconfirmed). Sensors are read *before* `lora_init()`/join, since
  a 2-3 s `measure_run()` after the join would land inside a JoinAccept/RX
  window; a total sensor failure is no longer fatal to the test — it sends the
  all-sentinel frame, which still decodes cleanly (2026-09-01)

### Changed
- **D5 = P1.07 is `LORA_RF_SW1`, not a spare GPIO.** The Wio-SX1262 brings its
  antenna-switch control out to that header pin. Firmware never drives it (the
  radio is `dio2-tx-enable`, so the SX1262 switches the path itself), but a
  pull-up or button there parks the switch and bills the System OFF budget.
  Corrected in the overlay comment, ARCHITECTURE, TRD, and the carrier README,
  all of which previously called D5 unused
- **Web config app rebuilt** — the `index2.html` redesign is now `index.html`
  (three-language DE/EN/AR, guided setup wizard, `device.svg` / `logo.svg`
  assets); the old `index2.html` is gone and the command encoder moved to its
  own page
- `docs/POWER_BUDGET.md` gains a third thing to check before blaming firmware
  for a sleep-current regression: **the carrier board**. A unit read 54 µA on
  2026-08-29 and was chased through the firmware for hours — wake-port choice,
  errata workarounds, A/B builds — before a carrier swap dropped it to 8 µA. The
  XIAO alone measured 3.7 µA throughout, which was the tell
- All build and flash scripts are now repo-relative — no absolute paths
- Vendored Seeed platform trimmed from 33 MB to the single nRF54LM20A board
  definition, under `third_party/seeed-xiao-nrf54lm20a/`
- Application devicetree overlay moved to `boards/`
- Documentation moved to `docs/` and re-pointed from the nRF54L15 to the
  nRF54LM20A; TRD gained an explicit "deviations from this TRD" section

## [0.9.0] — 2026-07-25 — nRF54LM20A port

### Changed
- **Target module: XIAO nRF54L15 → XIAO nRF54LM20A.** The carrier PCB is
  unchanged; both modules share the XIAO footprint and pinout
- nRF Connect SDK v3.2.4 → **v3.4.0** (Zephyr 4.2 → 4.4)
- GPIO remap — header *roles* preserved 1:1, underlying pins all changed; header
  SPI is now `spi23` (`spi00` is the on-board NOR)
- Battery sensing moved from an ADC divider + `vbat_pwr` gate to the on-board
  **nPM1300** factory-trimmed VBAT ADC — no divider, no calibration factor
- Console moved to `uart20` → CMSIS-DAP VCOM @115200 in **all** builds;
  `-S cdc-acm-console` removed (the nRF's USBHS CDC never enumerates on this board)
- DS18B20 resolution 10-bit → 9-bit (94 ms vs 187 ms conversion)
- `CONFIG_LORAMAC_REGION_*` → `CONFIG_LORAWAN_REGION_*` (Zephyr 4.4 rename)

### Fixed
- **Sleep current 19.4 µA → ~4 µA:** on-board py25q64 NOR now enters deep
  power-down before System OFF, and the nPM1300 auto-ADC (`IBAT_EN` +
  `TASK_AUTO`) is cleared — the PMIC keeps converting through the SoC's System OFF
- `pmic_i2c` pins corrected to SDA P1.18 / SCL P1.17 (board DTS declares
  P1.15/P1.16)
- `pmic_leds` disabled — the board DTS armed `led0-mode="error"`, which this unit
  would have sat in permanently
- Q_measure 16 → 12 mC, Q_tx 200 → 63 mC

### Security
- `charging-enable` **deleted** from `&pmic_charger`. This design runs a primary
  Li-SOCl₂ cell; with charging armed, applying VBUS would push charge current
  into a non-rechargeable lithium cell. `thermistor-ohms = <0>` since no NTC is
  fitted

## [0.8.0] — 2026-07-07 — 999 kg payload

### Changed
- Uplink payload 7 → **8 bytes**: weight widened to uint24 grams (0–999000),
  clamp raised from 65.535 kg. Temperature, battery, and flags shifted
- `last_tx_weight` and the anomaly baseline widened to match — they were silently
  capping deltas above 65 kg
- LiveSensors BLE notify widened 6 → 8 bytes
- Both TTN decoders now emit `null` for invalid sensors instead of a sentinel number

### Fixed
- Failed-sensor sentinel moved to `0xFFFFFF`, clear of the raised clamp — a
  maxed-out real reading and a dead sensor were previously indistinguishable
- **BEEP ingestion**: not a payload bug. BEEP routes on
  `payload_fields.hardware_serial` (TTN Stack **V2**) when given no explicit key;
  this application uses V3 webhooks, which carry the identifier at
  `end_device_ids.dev_eui`. Fixed externally by adding `?key=<DevEUI>` to the
  webhook URL

### Reverted
- `BT_GATT_PERM_WRITE_ENCRYPT` on the credential characteristics. Web Bluetooth
  has no `pair()` API; the OS-triggered SMP pairing hung mid-write until
  supervision timeout, and re-advertising then failed with `-ENOMEM`

## [0.7.0] — 2026-07-06 — field robustness

### Added
- `task_wdt` 60 s software channel backed by the `wdt31` hardware fallback, armed
  in `main()` and fed at every FSM boundary; long waits sliced ≤30 s
- `CONFIG_RESET_ON_FATAL_ERROR` + `CONFIG_HW_STACK_PROTECTION`; reset cause
  (watchdog / lockup / software) decoded and logged on boot
- `tx_pending` ZMS flag — a failed uplink retries on the *next* wake instead of
  waiting a full `tx_interval` in silence
- I2C bus recovery (`i2c_recover_bus()` + retry) on every NAU7802 transfer
- `measurement_data_t.valid` bitmask — a failed sensor is never transmitted as a
  real `0`
- `pm_static.yml` pinning `zms_storage`, and `sysbuild.conf` for OTA readiness

### Fixed
- DevNonce persistence now hard-fails the join rather than burning nonce 0
  repeatedly and draining the battery in a TTN-rejected retry storm
- `active_conn` mutex-guarded and `pending_cmd` made an atomic CAS gate — both
  were racy across the BT RX, system workqueue, and main threads
- `T_TX ≥ T_MEAS` cross-validated on both the downlink and BLE write paths

## [0.6.0] — 2026-06 — active power optimization

### Changed
- Measurement cycle 7.4 s / 133 mC → 420 ms / 6.76 mC (94% time, 95% charge)
- LoRaWAN DR3/SF9 instead of DR0/SF12 — roughly 8× shorter airtime
- NAU7802 PUR and CR-bit polling replace blind sleeps
- 5 s boot delay skipped on timer and button wakes

## [0.5.0] — 2026-06 — first complete node

### Added
- 4-state FSM, System OFF with GRTC timed wake (8 µA on the nRF54L15)
- NAU7802 weight, DS18B20 temperature, battery, SX1262 LoRaWAN OTAA to TTN
- BLE GATT configuration service (13 characteristics) + Web Bluetooth page
- Anomaly detection with delta-based transmission
