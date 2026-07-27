# TanenBase Implementation Plan

## Sprint Order

### Sprint 1: Sanity Check (LED Blink + Serial Log)
**Goal:** Confirm build environment, flash, serial output work.
**Files:**
- `src/main.c` — LED blink + LOG_INF message
- `prj.conf` — base config (GPIO, logging)
- `debug.conf` — debug overlay (LOG=y, shell)
- `prj_production.conf` — production overlay (LOG=n, power save)
- Devicetree overlay if needed for LED
**Build:** `.uild.ps1 debug`
**Expected Serial:**
```
*** Booting TanenBase v0.1.0 ***
[INF] main: TanenBase initialized
[INF] main: LED blink started
```
**AC:** LED blinks on P2.00, serial output visible on COM15

### Sprint 2: FSM Skeleton
**Goal:** 4-state FSM engine, state transitions logged, no real hardware interaction yet.
**Files:**
- `src/core/fsm.c` + `src/core/fsm.h` — FSM engine with states: SETUP, MEASUREMENT, TRANSMISSION, ACTIVE_SLEEP
- `src/main.c` — init FSM, run loop
- Kconfig: CONFIG_TANENBASE_FSM
**Build:** debug.conf
**Expected Serial:**
```
[INF] fsm: State: SETUP
[INF] fsm: State: MEASUREMENT
[INF] fsm: State: TRANSMISSION
[INF] fsm: State: ACTIVE_SLEEP
```
**AC:** FSM cycles through all 4 states, transitions logged

### Sprint 3: NAU7802 Weight Reading
**Goal:** Read weight from NAU7802 via I2C sensor API, display on serial.
**Files:**
- `src/drivers/weight.c` + `src/drivers/weight.h` — NAU7802 wrapper using sensor_sample_fetch/sensor_channel_get
- Devicetree overlay for I2C (P2.08 SCL, P2.07 SDA) + NAU7802 node
- CMakeLists.txt update for NAU7802 Zephyr module
- Kconfig: CONFIG_TANENBASE_WEIGHT
**Build:** debug.conf
**Expected Serial:**
```
[INF] weight: NAU7802 initialized
[INF] weight: Raw ADC: XXXXX, Weight: XX.XX kg
```
**AC:** Valid ADC readings from NAU7802, weight value changes with load

### Sprint 4: DS18B20 Temperature
**Goal:** Read temperature from DS18B20 via 1-Wire on P1.04.
**Files:**
- `src/drivers/temp.c` + `src/drivers/temp.h` — DS18B20 wrapper
- Devicetree overlay for 1-Wire + DS18B20 node
- Kconfig: CONFIG_TANENBASE_TEMP
**Build:** debug.conf
**Expected Serial:**
```
[INF] temp: DS18B20 initialized
[INF] temp: Temperature: XX.XX °C
```
**AC:** Valid temperature reading, reasonable range (15-45°C)

### Sprint 5: Battery ADC
**Goal:** Read battery voltage via ADC on P1.14 (AIN7) with VBAT enable via vbat_pwr regulator on P1.15. Note: Seeed schematic mislabels these pins — real wiring follows base board DTS.
**Files:**
- `src/drivers/battery.c` + `src/drivers/battery.h`
- Devicetree overlay for ADC channel + VBAT enable GPIO
- Kconfig: CONFIG_TANENBASE_BATTERY
**Build:** debug.conf
**Expected Serial:**
```
[INF] battery: VBAT enabled, ADC reading: XXXX mV (XX%)
```
**AC:** Voltage reading within expected range, VBAT enable toggles correctly

### Sprint 6: LoRaWAN OTAA Join ✓
**Goal:** SX1262 init, OTAA join to TTN, confirmed on TTN console.
**Status:** DONE — joined TTN successfully (DevAddr assigned).
**Files:**
- `src/drivers/lora.c` + `src/drivers/lora.h` — SX1262/LoRaWAN wrapper
- Devicetree overlay for SPI + SX1262 pins (DIO1, RST, BUSY, NSS)
- `src/config/config.c` + `src/config/config.h` — ZMS-based nonce persistence + credential loading
- `prj_credentials.conf` — AppKey, DevEUI, JoinEUI (Kconfig strings)
- Kconfig: CONFIG_TANENBASE_LORA
**Build:** `build.ps1 debug.conf` (auto-includes `prj_credentials.conf`)
**Key learnings:**
- Wio-SX1262 RF switch is DIO2-only — do NOT set `tx-enable-gpios` (blocks RX)
- `USE_LRWAN_1_1_X_CRYPTO=0` required for TTN MAC v1.0.3
- `CONFIG_LORAWAN_SYSTEM_MAX_RX_ERROR=500` for RX window margin
- `CONFIG_LORAWAN_NVM_NONE=y` — nonce persisted manually via ZMS (settings_zms broken on nRF54L RRAM)
- `LOG_MODE_IMMEDIATE` must be OFF during LoRaWAN — blocks RX windows
- `prj_credentials.conf` must be in OVERLAY_CONFIG or AppKey defaults to zeros
**AC:** Device appears as "joined" on TTN console ✓

### Sprint 7: Measurement Feature ✓
**Goal:** Orchestrate weight + temp + battery reading in one measurement cycle.
**Status:** DONE
**Files:**
- `src/features/measurement/measure.c` + `measure.h` — `measure_init()`, `measure_run()`, `measure_last()`
- FSM MEASUREMENT state calls `measure_run(&data)`
**AC:** All 3 sensors read in sequence, results stored for transmission ✓

### Sprint 8: Transmission Feature ✓
**Goal:** Build big-endian LoRaWAN payload, uplink to TTN, verify on TTN console.
**Status:** DONE
**Files:**
- `src/features/transmission/transmit.c` + `transmit.h` — 4-byte payload per TRD §6
- FSM TRANSMISSION state calls `transmit_run(measure_last())`
**AC:** Payload visible on TTN console, decoded correctly ✓

### Sprint 9: Power Management + Active Sleep ✓
**Goal:** Deep sleep between cycles, SX1262 powered down, ≤20µA target.
**Status:** DONE — 8µA in System OFF (PPK2 @ 3.9V on battery pads, USB disconnected).
**Files:**
- `src/core/power.c` + `src/core/power.h` — System OFF with GRTC timed wake
- `src/drivers/lora.c` — added `lora_sleep()` (Radio.Sleep warm-start)
- `src/core/fsm.c` — calls lora_sleep() before power_off()
- board overlay — disable unused peripherals/regulators
- Kconfig: CONFIG_POWEROFF, CONFIG_PM_DEVICE, CONFIG_HWINFO

**What was done to get from 7mA → 8µA:**

| Step | Change | Current after |
|------|--------|---------------|
| 1 | GPIO disconnect all app pins (`nrf_gpio_cfg_default`) + SPI suspend | 3.80 mA |
| 2 | Drive LED (P2.00) HIGH (active-low). Drive regulator enables to OFF: pdm_imu_pwr (P0.01), rfsw_pwr (P2.03), vbat_pwr (P1.15) | 3.14 mA |
| 3 | Move `z_nrf_grtc_wakeup_prepare()` to right before `sys_poweroff()` (fixed GRTC wake) | 3.14 mA (wake fixed) |
| 4 | Fix rfsw_ctl (P2.05): was driven HIGH (=ON), now LOW (=OFF). DTS says active-HIGH | 1.19 mA |
| 5 | Add `Radio.Sleep()` call before System OFF (SX1262 deep sleep via SPI) | 1.19 mA |
| 6 | Drive SX1262 CS (P1.10) + RST (P1.06) HIGH instead of disconnect — floating CS wakes radio from sleep into ~1.5mA standby | **8 µA** |

**Key learnings:**
- nRF54L series has no GPIO RETAIN registers — GPIO config is retained through System OFF by default
- SX1262 CS must stay HIGH in System OFF — floating CS glitches wake radio
- rfsw_ctl DTS says `GPIO_ACTIVE_HIGH`, not active-low — driving HIGH enables it
- GRTC wake must be set up last, right before sys_poweroff()
- NAU7802 already powered down after init/read (~0.9µA) — no change needed
- Board-level: measure via battery pads at 3.7-3.9V, USB disconnected (SAMD11 debugger draws ~1mA)

**Pre-sleep sequence:** `lora_session_save()` → `lora_sleep()` → `LOG_PANIC()` → SPI suspend → GPIO cleanup → GRTC wake → `sys_poweroff()`

**AC:** Sleep current 8µA ≤20µA ✓, GRTC timed wake works ✓, device reboots into SETUP ✓

### Sprint 10: BLE Configuration Service ✓
**Goal:** BLE GATT service for Web Bluetooth config (creds, calibration, intervals).
**Status:** DONE — Web Bluetooth connects, all characteristics R/W, live sensors, LoRa test, calibration workflow verified.
**Files:**
- `src/features/ble_config/ble_svc.c` + `ble_svc.h` — custom GATT service (13 characteristics)
- `web/index.html` — single-file Web Bluetooth config app
- `prj.conf` — BT_PERIPHERAL, SDC, max 1 conn
- `CMakeLists.txt` — conditional BLE/anomaly sources
- Kconfig: CONFIG_TANENBASE_BLE_CONFIG, CONFIG_TANENBASE_SETUP_TIMEOUT

**GATT characteristics (UUID base `544E4253-xxxx-4269-8000-544E42415345`):**

| UUID | Name | Type | Access |
|------|------|------|--------|
| 0x0001 | DevEUI | 8B hex | R/W |
| 0x0002 | JoinEUI | 8B hex | R/W |
| 0x0003 | AppKey | 16B hex | W |
| 0x0004 | ZeroOffset | int32 LE | R/W |
| 0x0005 | ScaleFactor | int32 LE | R/W |
| 0x0006 | TxInterval | uint32 LE (sec) | R/W |
| 0x0007 | MsInterval | uint32 LE (sec, ≥60) | R/W |
| 0x0008 | WeightThresh | uint16 LE (g) | R/W |
| 0x0009 | TempThresh | uint16 LE (cc) | R/W |
| 0x000A | CalibRef | uint32 LE (g) | R/W |
| 0x000B | CmdStatus | 2B [cmd,result] | Notify |
| 0x000C | ConfigCmd | 1B cmd | W |
| 0x000D | LiveSensors | 8B LE (widened 2026-07-07, was 6B) | Notify |

**ConfigCmd values:** 0x01=Done, 0x02=Tare, 0x03=Clear Session, 0x04=Test LoRa, 0x05=Calibrate

**Key learnings / bugs fixed:**
- `BT_LE_ADV_CONN` renamed to `BT_LE_ADV_CONN_FAST_1` in NCS 3.2.4
- RF switch must be enabled for BLE: P2.03 HIGH (power), P2.05 LOW (ceramic antenna)
- Scan response with device name required for Web Bluetooth device picker
- Live sensor reads must run in thread context (k_work), not timer ISR — bus fault on I2C/1-Wire from ISR
- Live timer deferred to CCC subscribe callback — early start blocks GATT discovery
- Tare/calibrate serialized via k_work to avoid NAU7802 race with live sensor reads
- LoRa test on dedicated 4KB thread — `lorawan_send()` deadlocks if run on system work queue
- `k_thread_join` on never-started thread → NULL deref crash — guarded with `lora_test_started` flag
- BLE shutdown: `shutting_down` flag prevents disconnect callback from re-advertising during cleanup
- Shutdown order: stop adv → disconnect → wait 500ms → settle 100ms → bt_disable → RF switch off
- SDC on nRF54L doesn't support Zephyr VS TX power HCI command — removed

**AC:** Settings modified via web app persist in ZMS ✓, live sensors stream ✓, tare/calibrate work ✓, LoRa test joins+sends ✓, clean BLE shutdown ✓

### Sprint 10.5: Active Power Optimization ✓
**Goal:** Reduce measurement cycle from 7.4s/133mC to <1s/<15mC.
**Status:** DONE — 420ms/6.76mC measurement, 730ms/9mC TX. **94% time / 95% charge reduction.**

**Changes made:**

| Optimization | File(s) | Savings |
|-------------|---------|---------|
| Skip 5s boot delay on timer/button wake | `src/main.c`, `src/core/power.c` | 5000ms |
| Wake reason caching (hwinfo consumed once) | `src/core/power.c` | correctness |
| PUR polling (vs blind 600ms sleep) | `src/drivers/weight.c`, driver init | ~300ms |
| DS18B20 10-bit resolution (vs 12-bit) | overlay `resolution = <10>` | ~562ms |
| CR bit polling (vs blind 50ms sleeps) | `src/drivers/weight.c` | ~150ms |
| DR3/SF9 LoRaWAN datarate (vs DR0/SF12) | `src/drivers/lora.c` | ~8x shorter TX airtime |

**PPK2 measurements (production build, no UART):**

| Phase | Before | After |
|-------|--------|-------|
| Measurement | 7.4s / 133mC @ 18mA | 420ms / 6.76mC @ 16mA |
| TX (SF9) | ~2s / 117mC @ 80mA | 730ms / 9mC @ 80mA |
| Sleep (System OFF) | 8µA | 8µA (unchanged) |

**Key technical details:**
- NAU7802 CR bit (PU_CTRL bit 5) polled over SW I2C — reads immediately when conversion ready
- CS bit (PU_CTRL bit 4) must be set after power-up to start ADC conversions
- PU_CTRL read-only bits (PUR=bit3, CR=bit5) must be masked when writing — read-modify-write corrupts chip state
- Full driver init (reset + PUR + config + internal calibration) kept at POST_KERNEL — required for gain-128 PGA stability
- No DRDY pin connected — CR polling over I2C is the only synchronization method
- 5 discard samples after power-up for chopper stabilizer settling at gain 128
- nRF54L series lacks PM idle support in Zephyr — CPU can't idle during k_sleep (CONFIG_PM is not settable)
- SX1262 driver init at POST_KERNEL causes unavoidable ~80mA/50ms TCXO warmup spike on every cold boot

**AC:** Measurement 420ms/6.76mC ✓, TX 730ms/9mC ✓, weight stable with CR polling ✓

### Sprint 11: Anomaly Detection
**Goal:** Weight delta detection, alert flag in LoRaWAN payload.
**Files:**
- `src/features/anomaly/anomaly.c` + `anomaly.h`
- Kconfig: CONFIG_TANENBASE_ANOMALY
**Build:** debug.conf
**AC:** Anomaly flag set when weight delta exceeds threshold

### Sprint 12: Full Integration + Production Validation
**Goal:** All features running together, production config, power verified.
**Build:** prj_production.conf
**AC:** Full cycle works, sleep current ≤20µA, TTN receives data, BLE config works

### Sprint 13: Field Robustness (Watchdog, Fault Handling, OTA Readiness) ✓
**Goal:** Close TRD 13.1/AC-10 gap (no watchdog existed) + fix concurrency/recovery bugs found in industrial-readiness review. Build-verified against NCS v3.2.4.
**Status:** DONE
**Files:**
- `src/core/watchdog.c/h` — `task_wdt` 60s channel + `wdt31` hw fallback, fed at FSM boundaries
- `src/main.c`, `src/core/fsm.c` — watchdog wiring, sliced (`sleep_with_feed`) long waits
- `src/core/power.c` — decode/log WATCHDOG/CPU_LOCKUP/SOFTWARE reset causes on boot
- `src/drivers/weight.c` — `i2c_recover_bus()` retry wrapper on every NAU7802 transfer; CR-poll switched `k_busy_wait`→`k_msleep` (was pinning CPU active for whole conversion train)
- `src/drivers/lora.c` — DevNonce persist hard-fail aborts join; downlink `T_TX≥T_MEAS` cross-validation
- `src/config/config.c/h` — new `tx_pending` ZMS flag (ID 17, retry failed TX next wake, not next heartbeat); hex-credential length/char validation
- `src/features/measurement/measure.c/h`, `src/features/anomaly/anomaly.c`, `src/features/transmission/transmit.c` — `valid` bitmask on `measurement_data_t`; failed sensor → sentinel in payload, excluded from anomaly delta + last-tx baseline
- `src/features/ble_config/ble_svc.c` — `active_conn` mutex + ref-snapshot (was racy across BT RX/sysWQ/main); `pending_cmd` atomic CAS (was silently overwritable); live-sensor work skipped during LoRa test (was risking missed RX windows); AppKey/DevEUI/JoinEUI encrypted-write requirement **tried and reverted same day** — Web Bluetooth can't complete OS-triggered SMP pairing mid-write, hung the link to a supervision-timeout disconnect (reason 8) + `-ENOMEM` on re-advertise. Back to plain R/W.
- `prj.conf` — `CONFIG_TASK_WDT`, `CONFIG_TASK_WDT_HW_FALLBACK`, `CONFIG_RESET_ON_FATAL_ERROR`, `CONFIG_HW_STACK_PROTECTION`; removed dead `CONFIG_PM=y` (no HAS_PM on nRF54L series, was a silent no-op)
- board overlay — enable `&wdt31` (was `disabled` in SoC DTS)
- `sysbuild.conf`, `pm_static.yml` — MCUboot/OTA readiness; `zms_storage` pinned so app-size changes / bootloader adoption don't relocate ZMS and brick joined devices

**Key learnings:**
- NCS `nrf/lib/fatal_error` already defines `k_sys_fatal_error_handler` — do not add a custom one (link error: multiple definition). `CONFIG_RESET_ON_FATAL_ERROR=y` is the whole fix.
- `CONFIG_PM` has no effect on nRF54L series (`HAS_PM` unset) — idle states are SoC-layer, not Kconfig-gated.
- Windows build dirs must be short — building under a deep temp/scratchpad path fails ninja with "opening dependency file ... No such file" (MAX_PATH).
- Partition Manager places `zms_storage` dynamically *after app* by default — any app-size change or MCUboot adoption without `pm_static.yml` silently relocates/wipes DevNonce+session+credentials.

**AC:** AC-10 (watchdog resets on ≥60s stall) ✓ — builds verified debug (FLASH 20.6%/RAM 32.2%) and production (FLASH 15.2%/RAM 27.7%) against NCS v3.2.4, not yet hardware-tested for actual WDT trip / I2C recovery / BLE race repro.

**Open follow-ups:** production MCUboot signing key not yet provisioned; BLE pairing UX (Web Bluetooth passkey flow) for encrypted cred writes unverified (moot — see Sprint 14, encrypted writes were reverted).

### Sprint 14: Payload Widening (999 kg) + BEEP Ingestion Investigation ✓
**Goal:** Weight field topped out at 65.535 kg (uint16 grams) — widen for realistic loaded-hive weight headroom, confirm temp already signed both ways.
**Status:** DONE — 8-byte payload live in production, confirmed working end-to-end including BEEP.
**Files:**
- `src/features/measurement/measure.h/.c` — `weight_g` widened `uint16_t`→`uint32_t`, clamp raised 65535→999000 (999 kg design ceiling)
- `src/config/config.h/.c` — `last_tx_weight` (anomaly delta baseline) widened to `uint32_t` to match; was silently capping deltas above 65 kg too
- `src/features/anomaly/anomaly.c` — `last_weight` local widened to match
- `src/features/transmission/transmit.c/.h` — payload 7→8 bytes: weight now 3 bytes (`sys_put_be24`, 0-999000 valid, `0xFFFFFF` = sensor invalid — moved sentinel safely clear of the raised clamp, fixing a latent collision where a maxed-out real 65.535kg reading and a dead sensor were indistinguishable on the wire); temp/battery/flags shifted accordingly. Temp was already `int16_t` centi-°C (±327°C) — no change needed, just repositioned.
- `src/features/ble_config/ble_svc.c` — LiveSensors BLE notify widened 6→8 bytes (u32 weight)
- `web/index.html`, `web/index2.html` — LiveSensors parsing updated to match (`getUint32` for weight)
- `ttndecoder/beepdecoder.js`, `ttndecoder/custumdecoder.js` — updated for 8-byte layout, now emit `null` (not a bogus sentinel number) for invalid-sensor fields

**BEEP false alarm:** immediately after this shipped, BEEP (`api.beep.nl/api/lora_sensors`) stopped receiving data — decoded perfectly in TTN console, nothing in BEEP. Root-caused via `github.com/beepnl/BEEP` + the user-supplied `ttndecoder/Beep-Sensor-data-API-v0.5.pdf`: BEEP determines which registered device a measurement belongs to via `payload_fields.hardware_serial` (TTN **Stack V2**) when no explicit `key` is present in the payload. This project's TTN application uses **Stack V3** webhooks, which place the device identifier at `end_device_ids.dev_eui` instead of inside `payload_fields` — so BEEP couldn't route the measurement to a device, independent of payload width or content. **The payload-widening was never the actual problem** — first suspected and even briefly (partially) reverted before the real cause was found.
**Fix (external, not firmware):** TTN Console → Integrations → Webhooks → BEEP webhook, base URL changed to include the key explicitly: `https://api.beep.nl/api/lora_sensors?key=<DevEUI>`. Confirmed working with the full 8-byte/999kg payload.

**Key learning:** when a downstream integration silently stops receiving data right after a payload change, don't assume the payload is the cause — check the integration's own routing/auth requirements first. TTN Stack V2→V3 field renames (`hardware_serial`→`end_device_ids.dev_eui`, `payload_fields`→`uplink_message.decoded_payload`) are a recurring trap for any pre-2021 integration doc.

**AC:** Weight capable of 999 kg ✓, temp confirmed signed both ways (was already correct, unchanged) ✓, BEEP ingestion confirmed working with new payload ✓.

### Sprint 15: XIAO nRF54LM20A Port + Sleep-Current Campaign ✓
**Goal:** Move off the XIAO nRF54L15 (development module) onto the XIAO nRF54LM20A
Standard, and get System OFF back under the 20 µA budget on the new module.
**Status:** DONE (2026-07-25) — building and running on NCS v3.4.0 / Zephyr 4.4.0.

**Why the port:** the LM20A is the module the product ships with. Both XIAO
modules are pin- and footprint-compatible, so the `Tanen_Base_pcb` carrier board
was unchanged — see `Tanen_Base_pcb/README.md`.

**What changed:**
- **Board/SDK:** `xiao_nrf54l15/nrf54l15/cpuapp` → `xiao_nrf54lm20a/nrf54lm20a/cpuapp`;
  NCS v3.2.4 → v3.4.0 (Zephyr 4.2 → 4.4). Board def is out-of-tree and needs
  `-DBOARD_ROOT` on every build.
- **GPIO remap:** XIAO header *roles* kept 1:1 (D0=1-Wire, D1=DIO1, D2=RST,
  D3=BUSY, D4=NSS, D6/D7=sw-I2C, D8–D10=SPI), but the underlying pins all moved.
  Header SPI is now `spi23` — `spi00` is the on-board py25q64 NOR on P2.0x and
  must never be driven from software.
- **Battery path replaced:** the L15 build read VBAT through a divider on an
  ADC pin gated by a `vbat_pwr` regulator. The LM20A has an on-board **nPM1300**
  PMIC; `battery.c` now reads its factory-trimmed VBAT ADC over bit-banged
  `pmic_i2c` (no divider, no calibration factor, no enable GPIO).
- **Board DTS bug:** the vendored board `.dtsi` puts `pmic_i2c` on P1.15/P1.16.
  Real wiring is SDA P1.18 / SCL P1.17 — corrected in the app overlay.
- **Kconfig rename:** Zephyr 4.4 renamed `CONFIG_LORAMAC_REGION_*` →
  `CONFIG_LORAWAN_REGION_*`.
- **Console:** `-S cdc-acm-console` **removed**. It repoints `zephyr,console` at
  the nRF's own USBHS CDC (VID 0x2fe3), which has never enumerated on this board
  — totally silent console. All builds now log over `uart20` (TX P1.11 /
  RX P1.10) → on-board CMSIS-DAP VCOM → COM port @115200. Those pins are not on
  the XIAO header, so this costs no usable I/O.
- **RF switch gone:** the L15 build sequenced `rfsw_pwr`/`rfsw_ctl` (P2.03/P2.05)
  around BLE. The LM20A drives its own antenna path, and those pins now belong to
  the NOR flash.
- **DS18B20** dropped 10-bit → 9-bit (94 ms vs 187 ms conversion; the 5 °C
  anomaly threshold has ample margin).

**Sleep current: 19.4 µA → ~4 µA.** Two independent leaks, both easy to regress:

| Fix | Current after |
|-----|---------------|
| Baseline after port | 19.4 µA |
| On-board py25q64 NOR into **deep power-down** (`flash_enter_dpd()`) before System OFF — the driver claims P2.01/02/04/05 and JEDEC-probes on every boot, waking the part | ~9 µA |
| nPM1300 auto-ADC off (`IBAT_EN` + `TASK_AUTO` cleared in `pmic_enter_sleep()`) — the PMIC sits on VBAT and keeps converting straight through the SoC's System OFF | **~4 µA** |
| `pmic_leds` disabled; board dtsi armed `led0-mode="error"`, which this unit would sit in permanently | (included above) |

Also measured: sleep current is **flat from 3.6 V to 5.0 V** — nPM1300 quiescent
dominates at µA loads, so a higher-voltage pack buys nothing in sleep and costs
proportionally more energy per day.

**Safety change:** `charging-enable` **deleted** from `&pmic_charger`. This unit
runs a primary (non-rechargeable) Li-SOCl₂ cell; with charging armed, applying
VBUS (debug USB, bench supply) would push charge current into it → venting/fire.
`thermistor-ohms = <0>` because no NTC is fitted (10k left the charger in a
permanent NTC fault). Never re-add `charging-enable` unless the cell on BAT is
genuinely rechargeable.

**Key learnings:**
- Build dir must be **short** on Windows — a deep temp path fails ninja with
  "opening dependency file … No such file" (MAX_PATH).
- `usbhs` needs `&vregusb { status = "okay"; }` + `CONFIG_REGULATOR`, or
  `nrf_usbhs_wrapper.c` fails on undeclared `__device_dts_ord_*`.
- Reconfiguring a build dir after a failed/mode-switched build dies on
  "malformed string literal in extra_kconfig_options.conf" (stale nrf_security
  `CONFIG_MBEDTLS_CONFIG_FILE` STRING entries in CMakeCache re-emitted unquoted)
  → `--pristine`. `build.ps1` now auto-pristines on mode change via a
  `build/.build_mode` stamp.
- Every kconfiglib **warning is fatal** — never assign a symbol whose dependency
  an overlay disables (e.g. `CONFIG_LOG_DEFAULT_LEVEL` in `prj.conf` vs
  `CONFIG_LOG=n` in `prj_production.conf`).
- Prefer `west flash`. OpenOCD's stdout is swallowed when run through the
  `flash_*.ps1` wrappers, and a post-mass-erase "clearing lockup after double
  fault" is the expected blank-chip state, not a failure.
- pyocd cannot program this part: its flash algorithm hard-faults (IPSR=3) after
  a successful erase, leaving the chip blank. OpenOCD with Seeed's board cfg
  (`nrf54lm20a-load`) is the working path.

**AC:** builds clean on NCS v3.4.0 ✓, all sensors read ✓, TTN join + uplink ✓,
BLE config ✓, System OFF ~4 µA ≤ 20 µA ✓.

### Sprint 16: Open-Source Release Prep ✓
**Goal:** make the repo publishable.
**Status:** DONE (2026-07-27).
- Apache-2.0 `LICENSE` + `NOTICE`; `README.md`, `CONTRIBUTING.md`, `CHANGELOG.md`
- Vendored Seeed platform trimmed 33 MB → the single LM20A board definition
  under `third_party/seeed-xiao-nrf54lm20a/` (Apache-2.0, attribution kept)
- All build/flash scripts made repo-relative with SDK auto-discovery
  (`scripts/ncs-env.ps1`, `$env:NCS_ROOT` / `NCS_VERSION` / `NCS_TOOLCHAIN`)
- App overlay moved to `boards/`; secrets kept out via
  `prj_credentials.conf.example` + `.gitignore`
- Docs re-pointed at the LM20A; `Tanen_Base_pcb/README.md` documents that the
  KiCad project is still named for the nRF54L15 and why that is correct
