# test_app: measure_loop

Standalone MEASUREMENT loop for PPK2 power profiling of TanenBase.

Same measurement sequence as `fsm_measure_and_decide` but stripped of LoRa,
BLE, anomaly, and System OFF. Runs forever, every `CONFIG_TANENBASE_TEST_INTERVAL_S`
seconds (default **5 s**). Works with USB unplugged — the loop never blocks on
serial.

## What the cycle does

Same as `fsm_measure_and_decide`, just without LoRa/anomaly:

```
boot           # GRTC wake → main() runs
measure_init   # NAU7802 PUA on, DS18B20 init, ADC init
measure_run    # weight + temp + battery, log values
weight_sleep   # NAU7802 PUD=0 (~1 µA)
power_off(5)   # System OFF, GRTC wake in 5 s → cold reboot
```

PPK2 trace: wake spike → ADC plateau → sleep drop → ~5 s System OFF floor
(~3 µA target) → next wake. Matches production trace 1:1.

## Build

From the repository root:

```powershell
.\build_test.ps1            # incremental
.\build_test.ps1 --pristine # clean rebuild
```

Build dir: `build_test/` (separate from main `build/`).

Or manually:

```powershell
west build -b xiao_nrf54lm20a/nrf54lm20a/cpuapp `
    -s test_apps/measure_loop -d build_test -- `
    -DBOARD_ROOT="$PWD/third_party/seeed-xiao-nrf54lm20a"
```

## Flash

```powershell
west flash -d build_test
```

To change interval without editing source:

```powershell
.\build_test.ps1 --pristine -- -DCONFIG_TANENBASE_TEST_INTERVAL_S=10
```

## Serial log

Console is **uart20** (TX P1.11 / RX P1.10) -> on-board CMSIS-DAP VCOM -> COM
port @115200. Do *not* add `-S cdc-acm-console`: on this board it repoints the
console at the nRF's own USBHS CDC, which never enumerates, and the log goes
silent.

```
[00:00:05.123] <inf> test_meas: === TanenBase test_app: measure_loop ===
[00:00:05.124] <inf> test_meas: Interval: 5 s
[00:00:05.250] <inf> config: Config loaded (...)
[00:00:05.255] <inf> test_meas: >>> Cycle 1
[00:00:06.480] <inf> measure: Weight=0 g, Temp=23.45 C, Battery=3712 mV
[00:00:11.500] <inf> test_meas: >>> Cycle 2
...
```

## PPK2 setup

1. Disconnect USB from XIAO.
2. PPK2 in **Source meter** mode, **3.3 V** (or 3.7 V to emulate Li-ion).
3. Wire `VOUT` → battery pad `+`, `GND` → board GND. **Do not** also power via USB.
4. Flash test firmware with USB connected, then unplug USB before starting capture.
5. Start logging in nRF Connect → Power Profiler. Expect ~5 s idle floor + brief
   measurement spikes every cycle.

> Tip: leave RTT viewer attached over SWD if you want logs without UART current cost —
> UART backend adds ~200 µA average due to TX activity each cycle.

## Switching back to the main app

Two separate build dirs — switching is just a different `west` invocation. No file
edits needed.

```powershell
# Main app
.\build.ps1                    # builds into build/
west flash -d build

# Test app
.\build_test.ps1               # builds into build_test/
west flash -d build_test
```

If you only have one build dir (`build/`) and want to flip:

```powershell
# To main
west build -b xiao_nrf54lm20a/nrf54lm20a/cpuapp --pristine `
    -- -DBOARD_ROOT="$PWD/third_party/seeed-xiao-nrf54lm20a" `
       -DOVERLAY_CONFIG="prj_credentials.conf;debug.conf"
west flash

# To test app
west build -b xiao_nrf54lm20a/nrf54lm20a/cpuapp --pristine `
    -s test_apps/measure_loop `
    -- -DBOARD_ROOT="$PWD/third_party/seeed-xiao-nrf54lm20a"
west flash
```

## Files

- `src/main.c` — 5 s loop
- `CMakeLists.txt` — pulls `config.c`, `weight.c`, `temp.c`, `battery.c`, `measure.c` from `../../src/`
- `prj.conf` — sensors + UART, no LoRa/BLE/PM
- `Kconfig` — stub LoRa creds + `TANENBASE_TEST_INTERVAL_S`
- `boards/xiao_nrf54lm20a_nrf54lm20a_cpuapp.overlay` — copy of the app overlay in `boards/` (same hardware)
