# Contributing to TanenBase

Thanks for looking. This is a small, field-deployed project — the bar is "does it
survive a winter on a battery in a hive", so most of what follows is about not
breaking that.

## Before you start

Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). The dependency rules and the
single-pass FSM design are not incidental; a change that violates them usually
shows up later as a power regression rather than a compile error.

## Setup

You need nRF Connect SDK **v3.4.0** (Zephyr 4.4) and its bundled toolchain.

```powershell
Copy-Item prj_credentials.conf.example prj_credentials.conf   # gitignored
.\build.ps1 debug --pristine
```

If the SDK is not at `C:\ncs`, set `$env:NCS_ROOT`, `$env:NCS_VERSION`, or
`$env:NCS_TOOLCHAIN`.

## Ground rules

**Never commit credentials.** `prj_credentials.conf` is gitignored and must stay
that way. Real DevEUIs, JoinEUIs, and AppKeys do not belong in this repository,
in issues, or in logs pasted into issues.

**Do not re-enable nPM1300 charging.** `boards/*.overlay` deletes
`charging-enable` on purpose. The deployed design runs three primary,
non-rechargeable AAA lithium (Li/FeS₂) cells in series — charging them vents or
ignites them. If you are building a rechargeable variant, change the cells and
the overlay together, in the same commit, with a comment saying so.

**Check `docs/POWER_BUDGET.md` §5.2 before touching the BAT rail.** That pack
sits at 5.2 V fresh, above the nPM1300's 4.45 V recommended VBAT maximum and
0.3 V under its absolute maximum — an accepted, documented deviation. Any change
that raises BAT further is not acceptable.

**Do not touch P2.00–P2.05 from software.** That is the on-board py25q64 NOR.
`power.c` has a DO-NOT-TOUCH list; adding pins to it is fine, driving them is not.

**Keep ZMS access in `config.c`.** No other module reads or writes storage. The
DevNonce lives there, and a lost DevNonce means TTN replay-rejects every join
attempt — the node is bricked until it is re-registered.

**Do not move `zms_storage`.** `pm_static.yml` pins it at 0x1D1000 for exactly
that reason. Any app-size change would otherwise relocate it.

## Changes that need extra care

| If you change… | Also update… |
|---|---|
| The uplink payload | **Both** decoders in `ttndecoder/`, `web/index*.html`, and the payload table in `docs/ARCHITECTURE.md` |
| The BLE GATT service | `web/index.html`, the characteristic table in `docs/PLAN.md` |
| Anything in the sleep path | Re-measure with a PPK2 and update `docs/POWER_BUDGET.md` |
| Timing or interval logic | Check `T_TX ≥ T_MEAS` still holds on both the BLE and the downlink write paths |
| Kconfig defaults | The flag table in `docs/ARCHITECTURE.md` |

## Power regressions

This is the failure mode that matters most, and it is invisible without
instrumentation. If you touch `power.c`, any driver's shutdown path, or the
devicetree, measure before and after with a PPK2 at 3.6 V on the battery pads,
USB disconnected.

Thresholds from [`docs/POWER_BUDGET.md`](docs/POWER_BUDGET.md) §9:

- Sleep above **6 µA** → something is leaking. First two suspects, both of which
  have caused this before: the NOR flash not in deep power-down
  (`flash_enter_dpd()`), and the nPM1300 auto-ADC re-armed (`pmic_enter_sleep()`).
- Measurement above **15 mC** or **1.9 s** → ADC settle or calibration regression.
- TX above **85 mC** or **9 s** → a join attempt, an SF12 fallback, or an RX2
  timeout.

## Style

Match the file you are editing. Broadly: Zephyr conventions, tabs in devicetree,
`LOG_*` macros only (never `printk`), driver functions return `0` or a negative
errno, and comments explain *why* — the codebase is full of "we tried the obvious
thing and here is how it failed" notes, which are the most valuable comments in
it. Please keep writing those.

## Submitting

1. Branch from `main`.
2. Build both configurations: `.\build.ps1 debug --pristine` and
   `.\build.ps1 prod --pristine`. Both must be clean — every kconfiglib warning
   is fatal in this tree.
3. Say in the PR what you tested on real hardware, and what you did not. "Builds
   clean, not hardware-tested" is a perfectly acceptable and useful statement;
   silently implying otherwise is not.
4. If you changed anything in the sleep or measurement path, include the PPK2
   numbers.

## Reporting bugs

Include the board revision, the NCS version, the build mode (debug/prod), and the
console output over uart20 at 115200. For anything LoRaWAN-related, the TTN
console event log is usually more informative than the device log.

If the symptom is "data arrives at TTN but not at my backend", check the webhook
URL before anything else — see the BEEP note in the README.
