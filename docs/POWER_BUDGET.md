# TanenBase — Power Budget Reference

PPK2 baseline @ Vbat = 3.6 V. Use for battery ETA calc & regression detection.
Date: 2026-09-10 (pack changed to **3× AAA 1.5 V Li/FeS₂ in series** — 1200 mAh,
4.5 V nominal, **5.2 V measured fresh**. See §5.2: this is the configuration §5.1
had ruled out on the input-voltage window, deployed anyway with the margin
documented. Charge/current figures below are unchanged 2026-07-25 PPK2 captures
at 3.6 V; they have **not** been re-measured on the 4.5 V pack).

---

## 1. Per-state consumption

| State | Avg I | Peak I | Duration | Charge | Power |
|---|---|---|---|---|---|
| **Sleep** (System OFF) | 4.5 µA † | — | continuous | 0.27 mC/min | 16.2 µW |
| **Measure** (sensor acq.) | 8.00 mA | 57.64 mA ‡ | 1.5 s | 12 mC | 28.8 mW |
| **TX** (LoRa uplink) | 9.26 mA | 91.70 mA ‡ | 6.8 s | 63 mC | 33.3 mW |

> Sleep includes NAU7802 in PUD power-down, SX1262 warm-start sleep (1.2 µA —
> loramac-node `RadioSleep()` hardcodes `WarmStart=1`), DS18B20 standby,
> py25q64 NOR in **deep power-down**, nPM1300 auto-ADC **off**, nRF54LM20A
> System OFF, GRTC retained. See `[[tanenbase-sleep-current]]` — the NOR-DPD and
> nPM1300 fixes are what took this from 19.4 µA, and both are easy to regress.
> Third thing to check before blaming firmware: **the carrier board itself**. On
> 2026-08-29 a unit read **54 µA** and the regression was chased through the
> firmware for hours — GPIO wake port, errata workarounds, A/B builds — before a
> carrier swap dropped it to 8 µA. The XIAO alone measured 3.7 µA the whole time,
> which was the clue: **if the module is clean, the leak is downstream.**
> Measure = NAU7802 init → per-wake internal offset cal → 10 SPS settle → cluster readout.
> TX = LoRa join check + uplink + RX1/RX2 windows.
> Avg I is charge ÷ duration.
>
> † **Flat across Vbat = 3.6 V → 5.0 V** (measured 2026-07-25, ~4.0 µA at 3.6 V,
> 4.5 µA used here as the design figure). Sleep current does *not* fall with
> rising input the way a buck-regulated load would — nPM1300 quiescent dominates
> at µA loads. Consequence: a higher-voltage pack buys nothing in sleep, and
> costs proportionally more *energy* per day (4.5 µA × 5 V vs × 3.6 V).
> ‡ Peaks carried over from the 2026-06-09 capture — **not re-measured**. §6 and
> the bulk-cap sizing depend on the TX peak; re-verify before PCB release.

---

## 2. Daily charge formula

```
N_wake   = 86400 / T_meas         # wakes/day
N_tx     = 86400 / T_tx           # TX wakes/day (heartbeat)
N_meas   = N_wake − N_tx          # measure-only wakes

Q_day = I_sleep × 86400
      + N_wake × Q_measure
      + N_tx   × Q_tx

mAh/day = Q_day_mC / 3600
```

Constants @ 3.6 V:
- `I_sleep = 4.5 µA`
- `Q_measure = 12 mC`
- `Q_tx = 63 mC`

---

## 3. ETA table — common schedules

| T_meas | T_tx | Sleep | Measure | TX | **Total mAh/day** |
|---|---|---|---|---|---|
| 15 min | 1 h | 0.108 | 0.320 | 0.420 | **0.848** |
| 15 min | 2 h | 0.108 | 0.320 | 0.210 | **0.638** |
| 15 min | 4 h | 0.108 | 0.320 | 0.105 | **0.533** |
| 15 min | 6 h | 0.108 | 0.320 | 0.070 | **0.498** |
| 30 min | 2 h | 0.108 | 0.160 | 0.210 | **0.478** |
| 30 min | 4 h | 0.108 | 0.160 | 0.105 | **0.373** |
| 30 min | 6 h | 0.108 | 0.160 | 0.070 | **0.338** |
| 60 min | 2 h | 0.108 | 0.080 | 0.210 | **0.398** |
| 60 min | 4 h | 0.108 | 0.080 | 0.105 | **0.293** |
| 60 min | 6 h | 0.108 | 0.080 | 0.070 | **0.258** |

> Sleep floor = 0.108 mAh/day. Below this is impossible without sleep-current improvement.

---

## 4. Energy share (15 min / 2 h, baseline)

```
Measure  50%  ██████████████████████████████
TX       33%  ████████████████████
Sleep    17%  ██████████
```

⚠️ **This conclusion flipped in this revision.** TX used to be 55% of the budget;
with Q_tx down 200 → 63 mC it is now 33%, and **Measure dominates at 50%**.
Stretching T_meas is now the biggest single lever — 15 → 30 min saves
0.160 mAh/day, while halving the TX rate (2 h → 4 h) saves only 0.105.

---

## 5. Battery life @ 0.638 mAh/day (15 min / 2 h)

| Battery | V_nom | Cap | Usable (90 %) | Energy life | SD-cap |
|---|---|---|---|---|---|
| **3× AAA Li/FeS₂ series** | **4.5 V** (5.2 V fresh) | **1200 mAh** | **1080** | **4.6 y** | **~20 y** ⭐ deployed |
| ER14505 AA Li-SOCl₂ | 3.6 V | 2400 mAh | 2160 | 9.3 y | ~10 y (previous choice, §5.1) |
| Tadiran TLP-93111 (HLC AA) | 3.6 V | 2100 mAh | 1890 | 8.1 y | ~10 y, no passivation |
| ER17505 A Li-SOCl₂ | 3.6 V | 3600 mAh | 3240 | 13.9 y | ~10–15 y |
| ER26500 C Li-SOCl₂ | 3.6 V | 8500 mAh | 7650 | 32.8 y | ~15 y |
| ER34615 D Li-SOCl₂ | 3.6 V | 19000 mAh | 17100 | 73.4 y | ~20 y |
| 18650 Li-ion | 3.7 V | 2500 mAh | 2250 | 9.7 y | ~5 y (cycle) |
| LiPo 1S 1000 mAh | 3.7 V | 1000 mAh | 900 | 3.9 y | ~3 y (cycle) |

> SD = self-discharge. Real life is `min(energy life, SD-cap)`.
> Li-SOCl₂ self-discharge ~1 %/year; Li/FeS₂ likewise <1 %/year, ~20 y shelf.

The deployed AAA pack is the one row that is **energy-capped, not SD-capped**:
4.6 y of charge against a ~20 y shelf life. Capacity is now the binding
constraint, which it never was with the Li-SOCl₂ options — every one of those
outlived its own ~10 y self-discharge limit.

Life by schedule, deployed pack (1080 mAh usable, §3 daily figures):

| T_meas / T_tx | mAh/day | Life |
|---|---|---|
| 15 min / 1 h | 0.848 | 3.5 y |
| **15 min / 2 h** | **0.638** | **4.6 y** ← baseline |
| 15 min / 4 h | 0.533 | 5.5 y |
| 30 min / 2 h | 0.478 | 6.2 y |
| 30 min / 4 h | 0.373 | 7.9 y |
| 60 min / 4 h | 0.293 | 10.1 y |
| 60 min / 6 h | 0.258 | 11.5 y |

> These are **conservative**: they carry the 3.6 V charge figures straight over.
> If the nPM1300 buck scales active current as `I_bat ∝ 1/V_in` (§7), the same
> schedules land ~20 % better — 5.6 y at the baseline. Unverified on this pack;
> do not quote the higher number until a PPK2 run at 4.5 V confirms it.

### 5.1 Why AA Li-SOCl₂ and not 1.5 V lithium (decision, 2026-07-25 — superseded by §5.2)

Constraint: **no additional ICs** — no buck, no LDO. The pack therefore drives
the XIAO BAT pin, i.e. the nPM1300 VBAT input, directly:

| | Value |
|---|---|
| nPM1300 VBAT **recommended operating** | **2.3 – 4.45 V** |
| nPM1300 VBAT absolute maximum | 5.5 V |

1.5 V AA cells (Varta / Energizer L91 lithium Li/FeS₂, or alkaline) have **no
series count that fits that window unregulated**:

| Config | Fresh OCV | EOL | Verdict |
|---|---|---|---|
| 3× series | **5.4 V** (1.8 V/cell OCV) | 3.0 V | over the 4.45 V limit; 0.1 V under abs max |
| 2× series | 3.6 V | **2.0 V** | drops under the 2.3 V minimum — loses the pack tail |
| parallel | **1.5 V** | 1.0 V | far under the 2.3 V minimum |

Note 1.5 V is the *nominal* figure — a fresh Li/FeS₂ AA sits at ~1.8 V
open-circuit, and at a 4.5 µA sleep load the pack is effectively unloaded, so
OCV is what the PMIC actually sees. Worst at low temperature, where OCV is
highest.

→ **The cell must be natively 3.6 V.** In AA form that is Li-SOCl₂ (ER14505),
which lands mid-window at 3.67 V fresh / 3.0 V EOL.

Parallel ER14505 was evaluated and rejected: 3 cells give ~28 y of energy but
the ~10 y self-discharge cap dominates, so the gain over a single cell is ~0.7 y
— and blocking diodes (needed to stop cross-charging) leak ~0.5 µA each, adding
~1.5 µA to a 4.5 µA sleep budget. Not worth it.

### 5.2 Deployed pack: 3× AAA 1.5 V Li/FeS₂ (2026-09-10)

The field units run **three AAA lithium-metal (Li/FeS₂) cells in series**:
1200 mAh, 4.5 V nominal, UL-certified with leak-proof seal and short-circuit
protection, <1 %/year self-discharge, −40 … +60 °C. **Measured fresh pack OCV:
5.2 V.**

This is the configuration §5.1 rejected. It is deployed with the deviation
recorded rather than hidden:

| | Value |
|---|---|
| Fresh pack (measured) | **5.2 V** |
| nPM1300 VBAT recommended max | 4.45 V → **exceeded by 0.75 V** |
| nPM1300 VBAT absolute max | 5.5 V → **0.3 V of headroom (6 %)** |
| Plateau (3× ~1.5 V) | ~4.5 V — still 0.05 V over the recommended max |
| EOL (3× 1.0 V) | 3.0 V ✓ comfortably inside the window |

What this means in practice:

- The part is **outside its recommended operating range from fresh until the
  pack falls under 4.45 V**, i.e. below ~1.48 V/cell. On the Li/FeS₂ plateau
  that is most of the pack's life, not a brief top-of-charge excursion.
- It stays under the 5.5 V absolute maximum, so this is a
  spec-margin/lifetime-risk deviation, not an instant-destruction one. Nordic
  gives no accuracy or longevity guarantee above 4.45 V.
- **Cold makes it worse**: Li/FeS₂ OCV rises as temperature drops, and the
  quoted −40 °C capability means the pack can sit even higher than 5.2 V at a
  winter hive. Check fresh OCV cold, not just at room temperature.
- The **VBAT ADC reading is suspect near the top** — nPM1300's VBAT measurement
  range tops out around 5 V, so a fresh pack may read clipped or non-linear.
  Uplinked `bv`/`VBatt` values above ~5 V should not be trusted until this is
  measured (§10).

Mitigations available without adding an IC: put the cells in **after** a short
pre-drain so the pack starts nearer 4.5 V, or accept the deviation and treat
"fresh-pack readings" as indicative. The clean fix stays what §7 says it is — a
buck (TPS62840-class) — which the no-extra-ICs constraint still excludes.

What the change buys, against the ER14505 it replaces:

- **No passivation** — Li/FeS₂ has none, so the depassivation step and the
  first-TX sag risk (§6) simply go away.
- **Cold**: −40 °C rated, against Li-SOCl₂ degrading below −20 °C. This closes
  the old §10 cold-temperature question.
- **Pulse**: enormous margin over the 91.7 mA TX peak (§6), where the ER14505
  was the marginal row.
- **Cost**: it is what the deployed units are actually built with.

What it costs: life drops **9.3 y → 4.6 y** at the baseline schedule (1200 mAh
against 2400 mAh, §5), and the input-voltage deviation above.

---

## 6. Pulse / passivation notes

91.7 mA peak (TX burst) — **peak not re-measured this revision, see §1 ‡**:

| Cell type | Max continuous | Max pulse | OK for 91 mA peak? |
|---|---|---|---|
| **ER14505 AA bobbin** | **100 mA** | **200 mA** | **marginal — 470 µF bulk cap is mandatory** |
| ER17505 A bobbin | 180 mA | 400 mA | OK |
| ER26500 C bobbin | 230 mA | 400 mA | OK |
| ER34615 D bobbin | 250 mA | 500 mA | OK |
| Tadiran HLC | — | 5000 mA | excellent |
| 18650 Li-ion | 2000 mA+ | 5000 mA | excellent |
| **AAA Li/FeS₂ (deployed pack)** | **~1000 mA** † | **~2000 mA** † | **yes — 10× margin on the 91 mA peak** |
| L91 AA (Li/FeS₂) | 2000 mA | 3000 mA | excellent — voltage was the objection, see §5.1/§5.2 |

† AAA Li/FeS₂ figures are class values for the chemistry, **not read off the
deployed cell's datasheet** — confirm before quoting them anywhere else. Even at
half these numbers the margin is large.

⚠️ The **ER14505 was the marginal row** — 91 mA peak against a 100 mA continuous
/ 200 mA pulse rating — and that is the constraint the deployed AAA pack removes.
Keep the 470 µF low-ESR bulk cap anyway: it is also what holds the rail up
through the TX burst across three series cells and their contact resistance.

**Passivation** (Li-SOCl₂ only — **does not apply to the deployed Li/FeS₂
pack**): after long storage, R_int can rise from ~80 mΩ → 1–10 Ω → first-TX
voltage sag → join fail. Retained for anyone running the §5.1 cell. Mitigations:
- 470 µF low-ESR bulk cap on Vbat rail (mandatory, see above)
- **Tadiran PulsesPlus TLP-93111 (AA, HLC)** — drop-in AA replacement, no
  passivation by design, huge pulse margin; costs ~1.2 y of life (§5). Take this
  if units may sit in storage > 6 months before deployment.
- Depassivation cycle before deploy (load 50 mA for 30 s)

---

## 7. Regulator / dropper impact

Retained for reference — **not applicable to the frozen design** (§5.1: cell
drives BAT directly, no series elements).

### 2× Schottky in series

Vf at load current:
- sleep 4.5 µA → ~0.05 V × 2 = 0.1 V
- measure 8 mA → ~0.25 V × 2 = 0.5 V
- TX 9.3 mA → ~0.25 V × 2 = 0.5 V

If load is **buck-regulated**: I_bat ∝ 1/V_in.
- sleep × 1.03, measure × 1.16, TX × 1.16
- Reverse leak: 2× ~0.5 µA = 1 µA → +0.024 mAh/day
- **Net: +18 % daily** → 0.638 → 0.753 mAh/day
- ER14505: 9.3 y → 7.9 y

> ⚠️ Note the sleep row assumes buck scaling, which §1 † shows does **not** hold
> on this board at µA loads. In sleep a series dropper costs the full leak with
> no 1/V_in compensation.

### 3× 1.5 V cells (4.5 V) + 2× Schottky → ~3.6 V

**Rejected.** Schottky is a fixed drop, not a regulator: the pack still swings
5.2 V fresh → 3.0 V EOL, so BAT would land ~4.7 V fresh — over the nPM1300's
4.45 V limit anyway (§5.1), while the drop costs the pack tail at EOL. The
deployed pack therefore runs **direct to BAT with no series elements** and
carries the over-voltage instead (§5.2). A real buck (TPS62840-class) is still
the only clean fix; it adds an IC, which the design constraint excludes.

Daily energy = 0.638 mAh × 4.5 V ≈ **2.87 mWh/day** on the deployed pack
(0.638 mAh × 3.6 V ≈ 2.30 mWh/day on the §5.1 cell). Same charge, more
energy: §1 † is the reason a higher-voltage pack buys nothing in sleep.

> The pre-2026-07-25 revision quoted 2.35 mWh/day against 1.22 mAh/day, which
> does not reconcile (1.22 mAh × 3.6 V = 4.4 mWh). Recomputed here as
> Q_day × 3.6 V.

---

## 8. Input voltage window / brownout

**Hard limit — the XIAO BAT pin is the nPM1300 VBAT input: 2.3 V min,
4.45 V max recommended, 5.5 V absolute.** Anything connected to BAT must stay
inside that window across its whole discharge curve *and* at fresh open-circuit.

| Battery | Fresh OCV | EOL Vbat | In 2.3–4.45 V window? |
|---|---|---|---|
| **3× AAA Li/FeS₂ (deployed)** | **5.2 V measured** | 3.0 V | **✗ over the 4.45 V recommended max until the pack drops below ~1.48 V/cell — under the 5.5 V absolute max. Accepted deviation, see §5.2** |
| ER14505 (§5.1 choice) | 3.67 V | 3.0 V | ✓ mid-window throughout |
| Tadiran TLP-93111 | 3.67 V | 3.0 V | ✓ |
| ER26500 | 3.67 V | 3.0 V | ✓ |
| 2× 1.5 V cells series | 3.6 V | 2.0 V | ✗ under at EOL |

Downstream of the PMIC: nRF54L DCDC min 1.8 V ✓, SX1262 PA min ~1.8 V ✓. The
Wio-SX1262 sits on the XIAO's regulated 3V3, not on BAT — confirm this on the
module schematic before any BAT-side change.

---

## 9. How to use this doc

**For new schedule estimate**:
1. Pick T_meas + T_tx from §3 (or compute via §2 formula).
2. Life = `min(usable mAh / mAh/day, SD-cap years)` — for the deployed AAA
   pack, **1080 mAh** usable and a ~20 y self-discharge cap, so it is the
   energy term that binds (§5). For the §5.1 ER14505: 2160 mAh, ~10 y cap.

**For PPK regression check**:
- If next build shows sleep > **6 µA** → leak somewhere. First two suspects are
  the ones that caused the 19.4 µA regression: py25q64 not in DPD
  (`flash_enter_dpd()`), and nPM1300 auto-ADC re-armed (`pmic_enter_sleep()`).
  Then: floating GPIO, peripheral left on, missing `weight_sleep()`.
- If measure event > **15 mC** or > **1.9 s** → ADC settle / cal regression.
- If TX event > **85 mC** or > **9 s** → join attempt, SF12 fallback, or RX2 timeout. See `[[feedback-ppk-lora-debug]]`.

**For circuit changes**:
- Anything on the BAT rail → check §8 window first, at fresh OCV *and* EOL.
- Change Vbat chemistry → recheck §5.1/§5.2 (voltage), §6 (pulse), §8 (window).
- **Measure fresh pack OCV before connecting it**, cold if the site gets cold:
  the deployed pack already spends 5.2 V of its 5.5 V absolute budget (§5.2).

---

## 10. Open questions

- Re-measure TX/measure **peak** currents — §6 and the bulk-cap sizing still rest
  on the 2026-06-09 numbers, and the chosen ER14505 is the marginal row.
- Confirm on the XIAO nRF54LM20A schematic that the 3V3 pin is the nPM1300 buck
  output, not a BAT passthrough (§8).
- ER14505 vs Tadiran TLP-93111 AA — decided by expected storage time before
  deploy (§6). If > 6 months, take the Tadiran.
- ~~Cold-temp operation? Li-SOCl₂ degrades < −20 °C.~~ **Closed 2026-09-10** —
  the deployed pack is Li/FeS₂, rated −40 … +60 °C (§5.2).
- **Does the nPM1300 VBAT ADC read a 5.2 V pack correctly?** Its measurement
  range tops out near 5 V, so fresh-pack `bv`/`VBatt` values may be clipped or
  non-linear. Compare a DMM against the uplinked value on a fresh pack before
  anyone reads a battery curve off the platform (§5.2).
- **How long does the pack sit above the 4.45 V recommended max?** Log Vbat over
  a season: that number decides whether the deviation is a few weeks or most of
  the pack's life, and whether a buck moves back onto the table (§5.2, §7).
- Re-run the PPK2 capture **at 4.5 V** — every charge figure here is a 3.6 V
  measurement, and §5's conservative life numbers assume no buck scaling (§7).
- Bulk cap 470 µF confirmed low-ESR and rated for the 91 mA pulse?
- Depassivation step in the production/deploy procedure?

---

## Changelog

- **2026-09-10** — Pack changed to **3× AAA 1.5 V Li/FeS₂ in series** (1200 mAh,
  4.5 V nom, 5.2 V measured fresh). Life 9.3 y → **4.6 y** at 15 min / 2 h.
  Passivation risk and the cold-temperature open question drop out; the pulse
  margin goes from marginal to 10×. In exchange the pack runs **above the
  nPM1300's 4.45 V recommended VBAT maximum** for most of its life (§5.2) —
  documented deviation, 0.3 V under the absolute maximum. Currents/charges are
  still the 3.6 V captures; not re-measured at 4.5 V.
- **2026-07-25** — nRF54LM20A port. Sleep 5.29 → 4.5 µA (NOR deep power-down +
  nPM1300 auto-ADC off). Q_measure 16 → 12 mC / 1.28 → 1.5 s. Q_tx 200 → 63 mC /
  14.1 → 6.8 s. Energy share flipped: Measure now dominates, not TX. Battery
  frozen to ER14505 AA (§5.1) under a no-extra-ICs constraint. Added §8 input
  voltage window.
- **2026-06-09** — Q_measure/Q_tx revised; Measure now 10 SPS + per-wake internal
  offset cal.
