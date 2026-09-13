# TanenBase Architecture

Smart beehive monitoring system on XIAO nRF54LM20A (Standard, non-Sense) + Wio-SX1262, built with nRF Connect SDK v3.4.0 (Zephyr RTOS). Board def is out-of-tree in `third_party/seeed-xiao-nrf54lm20a` (passed via `-DBOARD_ROOT`).

---

## Hardware Platform

| Component | Part | Interface |
|-----------|------|-----------|
| MCU | Seeed Studio XIAO nRF54LM20A (Standard, non-Sense) | — |
| Radio | Wio-SX1262 | SPI (spi23) |
| Load Cell ADC | NAU7802 | software I2C (GPIO bit-bang) |
| Temperature | DS18B20 | 1-Wire |
| Battery / PMIC | Nordic nPM1300 (on-board) | software I2C |

---

## Pin Map

XIAO header roles are kept 1:1 with the original nRF54L15 build, so the same
carrier PCB fits both modules — only the underlying GPIO numbers differ. See
[`Tanen_Base_pcb/README.md`](../Tanen_Base_pcb/README.md).

| XIAO Pin | nRF54LM20A GPIO | Function |
|----------|-----------------|----------|
| D0 | P1.00 | DS18B20 data (1-Wire). **Wio-SX1262 rework required:** its K1 button + 10K pull-up ship on D0 and stop 1-Wire working entirely — trace must be cut, see Tanen_Base_pcb/README.md |
| D1 | P1.31 | SX1262 DIO1 |
| D2 | P1.30 | SX1262 RST **+ external TanenButton** (bare switch to GND). Armed as a System OFF wake source in `power.c`; the internal pull-up holds NRESET high while asleep, so the radio keeps sleeping |
| D3 | P1.29 | SX1262 BUSY |
| D4 | P1.03 | SX1262 NSS (SPI CS) |
| D5 | P1.07 | **LORA_RF_SW1** — Wio-SX1262 antenna-switch control. NOT a spare GPIO: driven RF control line, do not repurpose (a pull-up here parks the switch and bills the System OFF budget) |
| D6/TX | P1.08 | SW I2C SCL (NAU7802) |
| D7/RX | P1.09 | SW I2C SDA (NAU7802) |
| D8/SCK | P1.04 | SPI SCK (spi23) |
| D9/MISO | P1.05 | SPI MISO (spi23) |
| D10/MOSI | P1.06 | SPI MOSI (spi23) |
| D19 | P0.00 | *(free)* — held the external TanenButton until 2026-09-07, when it moved to D2. Bottom pad, never routed on carrier v0.1 |
| — | P0.09 | On-board TanenButton (wake from System OFF) |
| — | P1.22–P1.24 | RGB LEDs (active HIGH) |
| — | P1.17 / P1.18 | nPM1300 PMIC I2C — SCL / SDA |
| — | P2.00–P2.05 | on-board py25q64 NOR (spi00) — **never drive** |

**Board-DTS correction:** the vendored board `.dtsi` declares `pmic_i2c` on
P1.15/P1.16. The real wiring is SDA P1.18 / SCL P1.17 (matching Seeed's own
`zephyr-battery` example); the app overlay re-points it. `power_en` (P1.12) is
not connected on this module and is disabled in the overlay.

---

## Layer Diagram

```
┌─────────────────────────────────────────────────────────┐
│                        main.c                           │
│                (entry point, delegates to FSM)          │
└───────────────────────┬─────────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────────┐
│                    core/fsm.c                           │
│         (4-state FSM, drives all transitions)           │
│                                                         │
│   ┌──────────┐  ┌──────────────┐  ┌─────────────────┐  │
│   │ power.c  │  │  config.c    │  │   (fsm state)   │  │
│   │ (sleep,  │  │  (NVS load/  │  │  SETUP →        │  │
│   │  wakeup) │  │   save)      │  │  MEASUREMENT →  │  │
│   └──────────┘  └──────────────┘  │  TRANSMISSION → │  │
│                                   │  ACTIVE_SLEEP   │  │
│                                   └─────────────────┘  │
└───────────────────────┬─────────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────────┐
│                      features/                          │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐ │
│  │ measurement │  │transmission │  │   ble_config    │ │
│  │ measure.c/h │  │transmit.c/h │  │  ble_svc.c/h   │ │
│  └──────┬──────┘  └──────┬──────┘  └────────┬────────┘ │
│         │                │                   │          │
│  ┌──────▼──────────────────────────────────────────┐   │
│  │                  anomaly/anomaly.c/h             │   │
│  │            (weight delta threshold checks)       │   │
│  └──────────────────────────────────────────────────┘   │
└───────────────────────┬─────────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────────┐
│                      drivers/                           │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────┐  │
│  │ weight.c │  │  temp.c  │  │  lora.c  │  │battery │  │
│  │ NAU7802  │  │ DS18B20  │  │ SX1262/  │  │  ADC   │  │
│  │ wrapper  │  │ wrapper  │  │ LoRaWAN  │  │        │  │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └───┬────┘  │
└───────┼─────────────┼─────────────┼─────────────┼───────┘
        │             │             │             │
┌───────▼─────────────▼─────────────▼─────────────▼───────┐
│                       Hardware                          │
│     NAU7802 (I2C)   DS18B20 (1-Wire)   SX1262 (SPI)     │
│                    nPM1300 PMIC (I2C)                   │
└─────────────────────────────────────────────────────────┘
```

---

## Project Structure

```
TanenBase/
├── src/
│   ├── main.c                  # Entry point — arms watchdog, delegates to FSM
│   ├── core/
│   │   ├── fsm.c/h             # 4-state FSM engine
│   │   ├── power.c/h           # System OFF, GRTC wake, peripheral shutdown
│   │   └── watchdog.c/h        # task_wdt (60s) + WDT31 hw fallback
│   ├── config/
│   │   └── config.c/h          # ZMS load/save, defaults — sole ZMS owner
│   ├── drivers/
│   │   ├── weight.c/h          # NAU7802 over software I2C
│   │   ├── temp.c/h            # DS18B20 (sensor API, 1-Wire)
│   │   ├── lora.c/h            # SX1262 / LoRaWAN wrapper
│   │   └── battery.c/h         # Battery voltage via nPM1300 VBAT ADC
│   └── features/
│       ├── measurement/measure.c/h   # Weight + temp + battery orchestration
│       ├── measurement/tempcomp.c/h  # Load-cell thermal drift correction
│       ├── transmission/transmit.c/h # Payload build (big-endian) + uplink
│       ├── ble_config/ble_svc.c/h    # BLE GATT service (Web Bluetooth config)
│       └── anomaly/anomaly.c/h       # Weight/temp delta anomaly detection
├── boards/                     # App devicetree overlay for the target board
├── third_party/                # Vendored Seeed XIAO nRF54LM20A board def
├── test_apps/measure_loop/     # Standalone sensor bring-up app
├── web/                        # Web Bluetooth config page + downlink encoder
├── ttndecoder/                 # TTN payload formatters (unified BEEP + beelogger)
├── Tanen_Base_pcb/             # KiCad carrier board (schematic, PCB, gerbers)
├── scripts/                    # Toolchain discovery helper
└── *.conf, Kconfig, CMakeLists.txt, pm_static.yml, sysbuild.conf
```

---

## FSM States

Single-pass design — `fsm_run()` checks wake reason, executes one pass, ends in System OFF.

```
  power_get_wake_reason()
         │
    ┌────▼────┐ Button/Reset
    │  SETUP  │──────────────────────────────┐
    │ BLE+sensors ON, blocks up to timeout   │
    └────┬────┘                              │
         │ Done / timeout                    │
         ▼                                   │
  ┌──────────────┐ Timer wake ───────────────┘
  │ MEASUREMENT  │
  │ measure_init + measure_run               │
  │ meas_count++ → ZMS                       │
  │ anomaly_check()                          │
  └──────┬───────┘                           │
         │                                   │
    should_tx?                               │
    ┌────┴────┐                              │
    │ YES     │ NO ──────────────────────┐   │
    ▼         │                          │   │
  ┌──────────────┐                       │   │
  │ TRANSMISSION │                       │   │
  │ lora_init (only when needed)         │   │
  │ transmit_run(data, ext, flags)       │   │
  │ update last_tx_weight/temp           │   │
  │ lora_session_save + lora_sleep       │   │
  └──────┬───────┘                       │   │
         │                               │   │
         └───────────────┬───────────────┘   │
                         ▼
                   ┌───────────┐
                   │   SLEEP   │
                   │ power_off(ms_interval)  │
                   │ System OFF + GRTC wake  │
                   └───────────┘
```

| State | BLE | LoRa | Sensors | Next |
|-------|-----|------|---------|------|
| SETUP | ON | OFF | ON (live) | MEASUREMENT |
| MEASUREMENT | OFF | OFF | ON | TRANSMISSION or SLEEP |
| TRANSMISSION | OFF | ON | OFF | SLEEP |
| SLEEP | OFF | OFF | OFF | MEASUREMENT (timer) or SETUP (button/reset) |

**Delta reporting:** measure every `ms_interval`, TX only on weight/temp delta exceeding threshold or heartbeat (`tx_interval / ms_interval` cycles). LoRa radio only powered on when TX needed — saves ~1mA on measure-only wakes. With Extended Mode on, a TX wake first runs one BLE scan for the SwitchBot sensors (`ext_scan_once()`: bt_enable → active scan → bt_disable, ≤`CONFIG_TANENBASE_EXT_SCAN_TIMEOUT_MS`, ends as soon as every enabled sensor has sent T/H + battery) and only then calls `lora_init()`, so the scan can't straddle a join/RX window. Measure-only wakes never start BT, and BLE readings don't feed the anomaly check.

---

## Key Design Decisions

### LoRaWAN
- Class A, OTAA, EU868, TTN
- Built-in Zephyr join retry (no custom retry logic)
- Big-endian payload encoding
- Downlink handled in TRANSMISSION state — FPort selects config field, one field per downlink:
  - **10** tx_interval (uint32 BE sec) · **11** ms_interval (uint32 BE sec, ≥60)
  - **12** anomaly_weight_threshold (uint16 BE) · **13** anomaly_temp_threshold (uint16 BE)

### BLE
- Just-Works pairing only, max 1 connection
- Custom GATT service: 15 characteristics (creds, calibration, timing, thresholds, commands, live sensors, Extended Mode config + live BLE sensors)
- UUID base: `544E4253-xxxx-4269-8000-544E42415345`
- Web Bluetooth API consumed by `web/index.html` — no mobile app required. `web/encoder.html` is a sibling page that builds FPort 10–13 downlinks offline (no Bluetooth, no node)
- Active only in SETUP state (button/reset wake)
- No RF-switch handling needed on LM20A — the module drives its own antenna path (the L15 build had to sequence `rfsw_pwr`/`rfsw_ctl` on P2.03/P2.05; on LM20A those pins belong to the on-board NOR flash)
- Live sensor notify: 2s interval, deferred to CCC subscribe (no reads until client subscribes)
- Extended Mode (`features/ext_sensors/`): **ExtConfig** `0x000E` (R/W, 26 B = `ext_config_t`, ZMS id 20 — version, master switch, 3 × {MAC MSB-first, role, enable}) and **ExtLive** `0x000F` (notify, 18 B = 3 × {status, temp_cc, hum, batt %, rssi}, same 2 s timer, cached values only — never a sensor read). The role limits (≤2 in-hive, ≤1 outside, unique non-zero MACs) are enforced in firmware too: a violating write gets ATT `0x13`. The 26-byte write needs an ATT MTU above 23 — `BT_SMP`'s default ACL RX size of 69 gives 65, and Web Bluetooth hosts negotiate up. During SETUP a continuous active scan (window = interval = 60 ms) runs next to the web-app link — **never give it a shorter window**: the SDC raises a window < interval scanner that misses full windows to 2nd scheduling priority, above the peripheral link (3rd); window == interval runs at 4th priority and is interleaved around the link (enforced by a `BUILD_ASSERT` in `ext_sensors.c`). The scan runs independent of the master switch, so a sensor shows values before it is switched on; the device scans for the **saved** MACs only
- LoRa test runs on dedicated thread (lorawan_send deadlocks system work queue)
- Shutdown: stop adv → disconnect → 500ms wait → 100ms settle → bt_disable → RF switch off
- `ble_config_run()` blocks caller until CMD_DONE or timeout

### Power Management
- **Sleep target:** ≤20µA — achieved **~4µA** via System OFF + GRTC wake
- **Active target:** <15mC per measurement cycle — achieved 12mC
- SX1262 put into warm-start sleep before System OFF; CS (P1.03) and RST (P1.30) held HIGH (a floating CS glitches the radio back into ~1.5mA standby)
- On-board py25q64 NOR driven into **deep power-down** before sleep — skipping this alone costs ~14µA
- nPM1300 auto-ADC (`IBAT_EN` + `TASK_AUTO`) cleared before sleep — the PMIC sits on VBAT and keeps converting straight through the SoC's System OFF
- Battery voltage read on demand from the nPM1300's factory-trimmed VBAT ADC — no divider, no external enable GPIO
- All peripherals shut down before `sys_poweroff()`

- Battery pack: **3× BEVIGOR AAA 1.5 V Li/FeS₂ in series**, 1200 mAh, 4.5 V nominal, 5.2 V measured fresh (21 °C) — primary, non-rechargeable. ~4.6 y at 15 min / 2 h (~3.3 y with Extended Mode BLE sensors — the scan raises a TX wake from 63 to 139 mC). At µA drain the chemistry holds ~1.79 V/cell, so the pack runs **above the nPM1300's 4.45 V recommended VBAT maximum for essentially its whole life** and a worst-case fresh pack reaches 5.49 V against the 5.5 V absolute maximum: accepted deviation, [POWER_BUDGET.md](POWER_BUDGET.md) §5.2

See [POWER_BUDGET.md](POWER_BUDGET.md) for the full PPK2 baseline, daily-charge
formula, and battery ETA tables.

**Active power optimizations:**
- Boot delay (5s) skipped on timer/button wakes — only on hard reset (flash safety)
- Wake reason cached in `power.c` (hwinfo latch consumed on first read)
- NAU7802 PUR polling (vs blind 600ms sleep) — ~300ms faster power-up
- NAU7802 CR bit polling (PU_CTRL bit 5) — reads ADC only when conversion ready, no blind sleeps
- DS18B20 at 9-bit resolution (94ms vs 750ms at 12-bit; the 5°C anomaly threshold leaves plenty of margin)
- LoRaWAN DR3/SF9 (vs default DR0/SF12) — ~8x shorter TX airtime
- 5 discard samples after NAU7802 power-up for gain-128 PGA chopper settling

**Power budget (PPK2 measured @ 3.6V, production build, 2026-07-25):**

| Phase | Duration | Charge | Avg Current | Peak |
|-------|----------|--------|-------------|------|
| Sleep (System OFF) | continuous | 0.27 mC/min | 4.5 µA | — |
| Measurement | 1.5 s | 12 mC | 8.0 mA | 57.6 mA |
| TX (SF9, unconfirmed) | 6.8 s | 63 mC | 9.3 mA | 91.7 mA |
| TX + Extended Mode (BLE scan + uplink, 2026-09-13) | 11.1 s | 139 mC | 12.5 mA | 77.8 mA |

**Platform limitations (nRF54L series + Zephyr 4.4):**
- No PM idle — the SoC lacks `HAS_PM` and `cpu-power-states` DTS. `CONFIG_PM=y` is not even settable; do not re-add it
- CPU cannot gate HFCLK during `k_sleep()` windows — active current stays high for the whole measurement
- SX1262 TCXO warmup spike (~80mA/50ms) unavoidable on every cold boot from System OFF
- No DRDY pin — NAU7802 conversion synchronization via CR bit over SW I2C only

### Fault Tolerance & Watchdog

- **Watchdog:** `task_wdt` software channel, 60s timeout (TRD 13.1/AC-10), backed by hardware `wdt31` fallback (catches kernel/timer death itself). Armed first in `main()`, fed at every FSM state boundary. Long waits (SETUP's 180s BLE window, LoRa-test thread join, debug/fallback sleeps) are sliced ≤30s with feeds in between — an unsliced `k_sleep()` would trip the WDT.
- **Fatal errors:** `CONFIG_RESET_ON_FATAL_ERROR=y` routes HardFault/BusFault/oops through NCS's `nrf/lib/fatal_error` handler — logs then cold-reboots instead of parking the CPU. `CONFIG_HW_STACK_PROTECTION=y` traps stack overflow via Cortex-M33 PSPLIM before it corrupts a neighboring thread's stack. `power_get_wake_reason()` decodes and logs `RESET_WATCHDOG` / `RESET_CPU_LOCKUP` / `RESET_SOFTWARE` on boot for field diagnostics.
- **I2C bus recovery:** every NAU7802 transfer gets one `i2c_recover_bus()` (9 SCL clocks + STOP) + retry on error — recovers a bus wedged by a brownout/ESD glitch mid-transaction without a full reset. `weight.c` CR-bit polling uses `k_msleep(2)`, not `k_busy_wait`, so the CPU can idle during the ~100ms/sample conversion train.
- **TX retry (tx_pending):** a failed uplink (join failure, radio init error) sets `tx_pending` in ZMS so the *next* wake retries immediately instead of waiting a full `tx_interval` in silence. Cleared on successful TX.
- **DevNonce hard-fail:** `lora_init()` aborts the join if the DevNonce can't be persisted to ZMS — burning nonce 0 repeatedly gets every join replay-rejected by TTN, draining the battery in a retry storm. Join is skipped and retried via `tx_pending` next wake.
- **Downlink cross-validation:** both `tx_interval` and `ms_interval` downlinks (FPort 10/11) and the BLE `MsInterval`/`TxInterval` writes enforce `T_TX ≥ T_MEAS` (TRD 7.1) against the *other* live value, not just a static floor.
- **BLE concurrency:** `active_conn` is mutex-guarded (`conn_acquire()` ref-snapshot pattern) — previously racy across BT RX / sysWQ / main thread (use-after-free / double-unref risk on shutdown). `pending_cmd` uses an atomic CAS gate so a second BLE command can't silently overwrite one still in flight. Sensor I/O triggered over BLE (live readings, tare, calibrate) runs on a dedicated **preemptible** work queue (`sensor_wq`, `K_PRIO_PREEMPT(10)`), never the cooperative sysWQ: gpio-I2C bit-banging busy-spins (`i2c_bitbang.c` `i2c_delay()` is a spin loop), and with the NAU7802 absent every SCL edge spins out the 100 ms clock-stretch timeout — ~3.5 s per `measure_run()`, re-queued every 2 s. On the sysWQ that starved the BT RX thread and `main`: the web app dropped after ~5 s and the watchdog reset the node 60 s later, which then skipped SETUP on the reset wake (2026-09-13, bare board without load cell). A live read is skipped while the previous one is still pending and while `lora_test_running` (the test thread measures itself); the queue is drained before SETUP returns so it can't race the MEASUREMENT cycle. AppKey/DevEUI/JoinEUI GATT writes are **plain** read/write (TRD 12.1's "encrypted pairing required" was tried and reverted 2026-07-07 — Web Bluetooth has no `pair()` API and can't reliably complete an OS-triggered SMP pairing mid-write; it hung the link until supervision timeout (disconnect reason 8) and left `bt_le_adv_start` failing with `-ENOMEM` afterward). See OPEN_QUESTIONS.md item 10.
- **Sensor validity:** `measurement_data_t.valid` bitmask (`MEAS_VALID_WEIGHT/TEMP/BATTERY`) — a failed sensor is never sent as a real `0` (which would falsely trigger a confirmed weight-anomaly uplink). Invalid fields are sent as sentinels (weight `0xFFFFFF`, battery `0xFFFF`, temp `0x7FFF`) in the payload and excluded from anomaly delta checks and the last-tx baseline.

### ZMS Storage (RRAM)
- All config persisted via ZMS (Zephyr Memory Storage) on nRF54L RRAM
- `config.c` owns all ZMS access — write-through cache pattern
- No other module reads flash directly

| ZMS ID | Field | Type | Default |
|--------|-------|------|---------|
| 3 | dev_eui | 8B | Kconfig |
| 4 | join_eui | 8B | Kconfig |
| 5 | app_key | 16B | Kconfig |
| 6 | zero_offset | int32 | 0 |
| 7 | scale_factor | int32 | 1000 |
| 8 | tx_interval | uint32 | 14400 (4h) |
| 9 | ms_interval | uint32 | 300 (5min) |
| 10 | weight_threshold | uint16 | 2000g |
| 11 | temp_threshold | uint16 | 500 (5.00°C) |
| 12 | last_tx_weight | uint16 | 0 |
| 13 | last_tx_temp | int16 | 0 |
| 14 | measurement_count | uint32 | 0 |
| 15 | ocal1 | int32 | — (NAU7802 offset cal) |
| 16 | link_state | struct | uplink_count=0, forced_dr=-1 |
| 17 | tx_pending | uint8 | 0 |
| 18 | tempcomp_state | struct | t_eff_mdeg=0, primed=0 |
| 19 | tare_temp | int32 | 25000 (m°C) |

### Calibration
- Zero offset: tare reading captured via BLE command in SETUP state
- Scale factor: known reference weight set via BLE, computed and stored
- Both applied in `measure.c`: `grams = (raw - zero_offset) * 1000 / scale_factor`

### Temperature compensation
- `tempcomp.c` — `W_corr = W_raw + k_c · (T_eff − T_ref)`, with `T_eff` a
  one-pole lag filter (τ = 25 min) on the DS18B20 reading. Fixed-point Q16,
  no float. Measured effect on the reference cell: σ 102.2 g → 15.7 g
- Applied in `measure.c` after the scale-factor conversion, before the 999 kg
  clamp. Skipped (weight reported raw) when the temp read fails
- `T_eff` is persisted in ZMS because System OFF wipes RAM — a filter that
  re-seeds every cycle has no memory to lag with. `fsm.c` saves it once per
  cycle, not per `measure_run()`, so the 2 s BLE live view can't hammer ZMS
- Δt: configured `ms_interval` for the first sample of a wake (no wall clock
  across System OFF), measured uptime for later ones. A non-timer wake means
  the gap is unknown → drop `T_eff` and re-seed
- `T_ref` = `T_eff` at the BLE tare (ZMS 19), written through `tempcomp_tare()`
  so the in-RAM copy updates too — the live view would otherwise keep applying
  the pre-tare reference for the rest of the setup window. Tare returns status
  `4` if the offset stored but `T_ref` did not
- `k_c` is a property of the cell **and its mount** — see
  [`docs/load_cells/H40A-C3-0150/`](load_cells/H40A-C3-0150/)

### Logging
- `LOG_*` macros exclusively (no `printk`)
- `CONFIG_LOG=n` in `prj_production.conf` — zero logging overhead in production
- `debug.conf` enables `CONFIG_LOG=y` + `CONFIG_LOG_DEFAULT_LEVEL=4`

---

## Interface Contracts

### Drivers

All driver functions follow this contract:

```c
/* Init: returns 0 on success, negative errno on failure */
int weight_init(void);
int temp_init(void);
int lora_init(void);
int battery_init(void);

/* Read: takes output pointer, returns 0 or negative errno */
int weight_read(struct sensor_value *val);
int temp_read(struct sensor_value *val);
int battery_read_mv(int32_t *mv);
```

### Features

- Features communicate via **events/callbacks** — never direct function calls between feature modules
- FSM passes a context struct to each feature; features report results via return codes
- `measure.c` calls drivers, packages results, returns to FSM
- `transmit.c` receives measurement data from FSM context, builds payload, returns TX result
- `anomaly.c` is called post-measurement with latest weight; fires callback if threshold exceeded

### FSM

```c
/* Single entry point from main.c */
void fsm_run(void);

/* State transition driven by return codes */
typedef enum {
    FSM_STATE_SETUP,
    FSM_STATE_MEASUREMENT,
    FSM_STATE_TRANSMISSION,
    FSM_STATE_ACTIVE_SLEEP,
} fsm_state_t;
```

---

## Kconfig Flags

Defined in [`Kconfig`](../Kconfig). Feature toggles:

| Flag | Default | Purpose |
|------|---------|---------|
| `CONFIG_TANENBASE_BLE_CONFIG` | `y` | BLE GATT configuration service (SETUP state) |
| `CONFIG_TANENBASE_ANOMALY` | `y` | Delta/anomaly detection module |
| `CONFIG_TANENBASE_EXT_SENSORS` | `y` | Extended Mode: SwitchBot BLE T/H sensors (selects `BT_OBSERVER`) |
| `CONFIG_TANENBASE_EXT_SCAN_TIMEOUT_MS` | 8000 | Upper bound of the BLE scan on a TX wake |

Runtime defaults (all also settable at runtime over BLE or LoRaWAN downlink,
and persisted in ZMS — Kconfig only seeds first boot):

| Flag | Default | Purpose |
|------|---------|---------|
| `CONFIG_TANENBASE_DEV_EUI` | `""` | Fixed DevEUI; empty = derive per-device from SoC hardware ID |
| `CONFIG_TANENBASE_JOIN_EUI` | all-zero | LoRaWAN JoinEUI |
| `CONFIG_TANENBASE_APP_KEY` | all-zero | LoRaWAN AppKey — **set in `prj_credentials.conf`, never commit** |
| `CONFIG_TANENBASE_DEFAULT_TX_INTERVAL` | 14400 (4h) | Heartbeat uplink interval |
| `CONFIG_TANENBASE_DEFAULT_MS_INTERVAL` | 300 (5min) | Measurement interval (min 60s) |
| `CONFIG_TANENBASE_DEFAULT_WEIGHT_THRESHOLD` | 2000 g | Weight anomaly threshold |
| `CONFIG_TANENBASE_DEFAULT_TEMP_THRESHOLD` | 500 (5.00°C) | Temperature anomaly threshold |
| `CONFIG_TANENBASE_TEMPCOMP` | `y` | Load-cell thermal drift correction |
| `CONFIG_TANENBASE_TEMPCOMP_GAIN_MG_PER_K` | 17734 | k_c — per cell **and mount**; 0 = no correction |
| `CONFIG_TANENBASE_TEMPCOMP_TAU_S` | 1500 (25min) | Thermal lag time constant |
| `CONFIG_TANENBASE_TEMPCOMP_T_REF_MDEG` | 25000 (25°C) | Fallback T_ref before the first tare |
| `CONFIG_TANENBASE_SETUP_TIMEOUT` | 180 s | SETUP-state / BLE advertising window |
| `CONFIG_TANENBASE_DEBUG_LOOP_INTERVAL` | 180 s | Cycle delay when System OFF is skipped (`ms_interval=0`) |

---

## LoRaWAN Payload Format

Big-endian encoding. All fields fixed-width for deterministic decoder on TTN.

| Byte(s) | Field | Type | Unit |
|---------|-------|------|------|
| 0–2 | Weight | uint24 | grams, 0-999000 (999 kg design ceiling) |
| 3–4 | Temperature | int16 | 0.01 °C, signed both ways |
| 5–6 | Battery | uint16 | mV |
| 7 | Flags | uint8 | anomaly bits |

Total: 8 bytes per uplink (widened from 7 bytes on 2026-07-07 — uint16 grams topped out at 65.535 kg). Failed-sensor sentinels: weight `0xFFFFFF`, battery `0xFFFF`, temp `0x7FFF` — TTN decoder must map these to null, not a literal value. The weight sentinel sits far above the 999 kg clamp so a maxed-out real reading can't be mistaken for a dead sensor.

### Extended Mode blocks

With Extended Mode on, one 5-byte block follows byte 7 for each enabled SwitchBot sensor **heard** in this wake's scan, in slot order — frames are 8, 13, 18 or 23 bytes (EU868 DR0 allows 51). The base 8 bytes are unchanged, so decoders that only read bytes 0–7 keep working.

| Byte | Field | Type | Unit |
|------|-------|------|------|
| 0 | Descriptor | uint8 | bits 7–4 type (`1` = SwitchBot T/H) · bit 2 role (`1` = outside) · bits 1–0 slot |
| 1–2 | Temperature | int16 | 0.01 °C (sensor resolution 0.1 °C) |
| 3 | Humidity | uint8 | %RH |
| 4 | Battery | uint8 | %, `0xFF` = not received |

An enabled sensor that was not heard costs no airtime: its block is left out and flags **bit 3** (`ext_missing`) is set. The decoder derives the block count from the length alone (`(len − 8) / 5`); a length that is not 8 + 5n decodes the base frame and warns.

### Server-Side Decoding

`ttndecoder/tanen-decoder.js` is the formatter to install in TTN (Payload formatters → Uplink). It parses the frame once and emits both supported platforms' key sets into one `decoded_payload`, so a single formatter feeds both integrations:

| Reading | BEEP key | beelogger key | Unit |
|---------|----------|---------------|------|
| Weight | `weight_kg` | `Gewicht` | kg |
| Temperature | `t` | `TempOut` | °C |
| Battery | `bv` | `VBatt` | V |
| Outside temperature / humidity (BLE, role outside) | `t` / `h` | `TempOut` / `FeuchteOut` | °C / %RH |
| 1-Wire probe, when an outside BLE sensor is present | `t_0` | — | °C |
| Inside #1 temperature / humidity (lower slot) | `t_i` / `h_i` | `TempIn` / `FeuchteIn` | °C / %RH |
| Inside #2 temperature / humidity | `t_1` / — | `TempIn2` / `FeuchteIn2` | °C / %RH |

An outside BLE sensor takes over `t`/`TempOut`; without one, those stay the 1-Wire probe, exactly as before. BEEP has no second inside-humidity key, so inside #2's humidity reaches beelogger only.

Each server persists the keys it recognises and ignores the others; `flags`, `anomaly_weight`, `anomaly_temp`, `heartbeat`, `ext_missing`, `ext_count`, `t_1wire`, `h_i2` and the BLE battery levels `bat_out`/`bat_in1`/`bat_in2` (%) are diagnostics visible in TTN live data only. The BEEP keys keep the original decoder's quantisation (10 g / 10 mV), the beelogger keys carry full payload precision — same reading, different rounding, by design. The per-platform predecessors (`beepdecoder.js` = `custumdecoder.js`, `tanen-beelogger-decoder.js`) remain in the folder for reference and are no longer the ones to deploy.

Integration wiring differs per platform. BEEP needs `?key=<DevEUI>` on the webhook base URL (see the note below). beelogger's community server takes `https://community.beelogger.de/<username>/{/devID}/beelogger_log.php?Passwort=<password>&LORA=1` — `{/devID}` is substituted by TTN per uplink, so TTN device IDs are named `beelogger1`, `beelogger2`, … to match the registered stations, and because the password is shared across stations one webhook covers the whole application. Both paths are confirmed receiving from the same uplink.

**BEEP ingestion note (2026-07-07):** widening this payload initially appeared to break BEEP (`api.beep.nl/api/lora_sensors`) — decoded correctly in TTN console, but data never showed up in BEEP. That was a false lead. Root cause: BEEP's key-routing falls back to `payload_fields.hardware_serial` (a TTN **Stack V2** field) when no explicit `key` is given; this project's webhook uses TTN **V3**, which puts the device identifier at `end_device_ids.dev_eui` instead — so BEEP couldn't match the measurement to a registered device, regardless of payload width. Fixed by adding `?key=<DevEUI>` to the BEEP webhook URL in TTN Console (Integrations → Webhooks) — an external config change, not a firmware/decoder change. Reference: `ttndecoder/Beep-Sensor-data-API-v0.5.pdf`.

---

## Extension Points

### Adding a New Sensor
1. Add `drivers/<sensor>.c/h` implementing `<sensor>_init()` / `<sensor>_read()`
2. Add the source to `target_sources()` in `CMakeLists.txt`
3. Add acquisition call in `features/measurement/measure.c`, plus a `MEAS_VALID_<SENSOR>` bit
4. Extend payload format and the TTN decoder (`ttndecoder/tanen-decoder.js`) as needed — add the field under both platforms' key sets

### Adding a New Radio Protocol
1. Add `drivers/<proto>.c/h` wrapping the Zephyr radio API
2. Add `features/transmission/<proto>_transmit.c/h` for payload + uplink logic
3. Guard with a Kconfig flag; FSM selects transmission feature at compile time

### Adding a New Anomaly Algorithm
1. Implement in `features/anomaly/anomaly.c` behind a new internal function
2. Expose via the same `anomaly_check(weight_g)` interface already used by FSM
3. Select algorithm via Kconfig or runtime config setting

---

## Build Configurations

| File | Use |
|------|-----|
| `prj.conf` | Base — all features on, serial off, watchdog on |
| `debug.conf` | Overlay — LOG on (level 4), console on uart20 |
| `prj_production.conf` | Overlay — LOG off, size optimization |
| `prj_credentials.conf` | **Gitignored** — real LoRaWAN AppKey. Copy from `prj_credentials.conf.example` |
| `sysbuild.conf` | OTA — enables MCUboot via sysbuild (unverified on LM20A, see below) |
| `pm_static.yml` | Pins `zms_storage` @ 0x1D1000/36KB — required with sysbuild so MCUboot's partition layout doesn't relocate ZMS and wipe DevNonce/session/credentials |
| `boards/xiao_nrf54lm20a_nrf54lm20a_cpuapp.overlay` | Devicetree — SX1262, sw-I2C, 1-Wire, PMIC pin fix, sleep-current config |

Normal use is the wrapper scripts, which locate the SDK, set `BOARD_ROOT`, and
force `--pristine` on a mode change:

```powershell
.\build.ps1 debug           # console + logs on uart20 -> CMSIS-DAP VCOM @115200
.\build.ps1 prod            # LOG off, size-optimized
.\build.ps1 prod --pristine
.\build_test.ps1            # standalone sensor bring-up app (test_apps/measure_loop)
```

Equivalent raw `west` invocation (`$BR` = `-DBOARD_ROOT=<repo>/third_party/seeed-xiao-nrf54lm20a`):

```sh
west build -b xiao_nrf54lm20a/nrf54lm20a/cpuapp --no-sysbuild -- $BR \
    -DOVERLAY_CONFIG="prj_credentials.conf;prj_production.conf"
```

Do **not** add `-S cdc-acm-console`: it repoints `zephyr,console` at the nRF's
own USBHS CDC (VID 0x2fe3), which never enumerates on this board — the console
goes silent. The `--sysbuild`/MCUboot path is **unverified on LM20A**; on the
earlier L15 port it failed in MCUboot's `flash_map_extended.c`. Replace the dev
signing key (`SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`) before any real OTA deployment.

Flash scripts:
```powershell
# Firmware update only (preserves ZMS config/calibration in RRAM)
.\flash_update.ps1

# Factory reset (CTRL-AP mass erase — wipes ZMS config/calibration AND the
# DevNonce counter; reset join nonces in the TTN console afterwards)
.\flash_rest.ps1
```

---

## Dependency Rules

- `main.c` depends on `core/fsm` only
- `core/fsm` depends on `core/power`, `core/config`, and all features
- Features depend on drivers only (no cross-feature imports), except `ble_config` which calls `measure_run()` for live sensor reads and `lora_*` for LoRa test
- Drivers depend on Zephyr APIs only (sensor API, SPI, I2C, ADC, GPIO)
- `anomaly` depends on no driver directly — receives pre-read values from FSM context
- No module outside `core/config` touches ZMS
