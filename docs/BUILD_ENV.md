# 🛠️ Build Environment — Tanen Project

**Date:** 2026-08-03
**Purpose:** Document the development environment setup for TanenBase on the Hermes server

## Project

**TanenBase** — low-power LoRaWAN beehive monitor:
- **MCU:** Seeed Studio XIAO nRF54LM20A
- **Radio:** Wio-SX1262 LoRa (LoRaWAN Class A, OTAA, EU868)
- **Sensors:** NAU7802 (load-cell) · DS18B20 (temp) · nPM1300 (battery)
- **SDK:** Zephyr 4.4 via nRF Connect SDK v3.4.0
- **Sleep:** ~4 µA — **~9 years** on a single AA cell

## Repository Layout

| Path | Description |
|---|---|
| `src/` | Firmware — core/ (FSM, power, watchdog) + drivers/ + features/ |
| `boards/` | Application devicetree overlay |
| `third_party/` | Seeed XIAO nRF54LM20A board definition |
| `Tanen_Base_pcb/` | KiCad carrier board (schematic, layout, gerbers) |
| `web/` | Web Bluetooth config page (`index.html`) + downlink encoder (`encoder.html`) |
| `ttndecoder/` | TTN payload formatters |
| `test_apps/measure_loop/` | Standalone sensor bring-up |

## Server Environment (2026-08-03)

### Installed ✅
- `west 1.5.0` + all Python requirements (pyelftools, PyYAML, pykwalify, jsonschema, canopen, packaging, patool, psutil, pylink-square, pyserial, requests, semver)
- `ninja` + `cmake` (system)
- Python 3.13 (via venv `/opt/data/pylibs`)
- .NET 10 SDK (`/opt/data/dotnet`) — for AgOpenWeb
- **nRF Connect SDK v3.4.0** full in `/opt/data/ncs` (5.7GB — Zephyr 4.4 + nrf + nrfxlib + modules)
- **Zephyr SDK 1.0.1** in `/opt/data/zephyr-sdk-1.0.1` (829MB) — toolchain `arm-zephyr-eabi` (gcc 14.3.0) under `gnu/`

### ✅ Build verified (2026-08-03)
```bash
west build -b xiao_nrf54lm20a/nrf54lm20a/cpuapp -d build_test . \
  --no-sysbuild -- -DBOARD_ROOT=/opt/data/projects/tanen/third_party/seeed-xiao-nrf54lm20a
```
**Result:** SUCCESS — FLASH 11.95% / RAM 10.45% — `build_test/zephyr/zephyr.{elf,hex,bin}`

### Errors encountered & fixes
| Error | Fix |
|---|---|
| `No board named 'xiao_nrf54lm20a'` | Add `-DBOARD_ROOT=.../third_party/seeed-xiao-nrf54lm20a` (after `--`) |
| `ZEPHYR_EXTRA_MODULES not a valid module` | Don't use it — use BOARD_ROOT instead |
| `Unable to find 'x86_64-zephyr-elf' in .../gnu` | Toolchain must live at `gnu/arm-zephyr-eabi/` inside the SDK |
| MCUboot/sysbuild configure fail | Use `--no-sysbuild` (documented in README — sysbuild unsupported on LM20A) |

### Required later
- **KiCad** (for the PCB — optional on the server, more important on the user's machine)

## Build Commands

```bash
export PATH=/opt/data/pylibs/bin:$PATH
export ZEPHYR_BASE=/opt/data/ncs/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr

# From the project directory:
cd /opt/data/projects/tanen
west build -b xiao_nrf54lm20a/nrf54lm20a/cpuapp .
```

## Notes

- `build.ps1` / `flash_*.ps1` scripts are **Windows**-oriented — on Linux use west directly
- `prj_credentials.conf.example` → copy to `prj_credentials.conf` with your TTN keys
- sysbuild (MCUboot/OTA) is unverified on LM20A/NCS v3.4.0 — use `--no-sysbuild`

*Last updated: 2026-08-03 by Zena*
