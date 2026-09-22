# Load cells

TanenBase is not tied to one load cell. Which cell is under the hive is a
**runtime setting**, picked on the setup page (Scale → Load cell) and stored in
ZMS, so one firmware image serves every build of the frame.

The setting selects the two constants of the temperature correction
([`../../src/features/measurement/tempcomp.c`](../../src/features/measurement/tempcomp.c)):
a gain `k_c` in mg/K and a thermal lag `τ` in seconds. It does **not** touch the
zero offset or the scale factor — those come from the tare and calibrate steps
and are per unit regardless of the profile.

## Profiles

| id | Profile | `k_c` | `τ` | Status |
|---:|---|---:|---:|---|
| 0 | Generic / unknown | 0 mg/K | 1500 s | No correction. The weight follows the daily temperature — ≈350 g p-p on the reference frame. |
| 1 | Bosche **H40A-C3-0150** | 17734 mg/K | 1500 s | **Measured**, 40 h sweep + 144 h field validation → [`H40A-C3-0150/`](H40A-C3-0150/) |
| 2 | Steinberg **SBS-PF-150** | 0 mg/K | 1500 s | **Not characterised** — field run planned. |
| 3 | Custom | from the stored record | from the stored record | Your own measured gain and lag, entered on the setup page. |
| 4 | **Zemic L6E / L6E3** | 0 mg/K | 1500 s | **Not characterised.** |
| 5 | **TAL220 / TAL220B** | 0 mg/K | 1500 s | **Not characterised.** |
| 6 | **Flintec PC / SB** | 0 mg/K | 1500 s | **Not characterised.** |

One entry per model, not per capacity: `k_c` is an absolute mg/K dominated by
the mount, so a 150 kg and a 200 kg version of the same cell in the same frame
land close enough that the variant is not worth a row.

A named-but-uncharacterised profile behaves **exactly** like generic — gain 0,
no correction. The label still earns its place: the unit records which cell is
bolted under the hive, and a later firmware can fill the constants in without
the beekeeper touching anything, because `config_get_lc()` resolves non-custom
profiles from this table on **every read** rather than from the stored copy.

The ids are the GATT contract (`LC_PROFILE_*` in
[`../../src/config/config.h`](../../src/config/config.h)) — **append, never
reorder.** That is why `LC_PROFILE_CUSTOM` sits at 3, in the middle: it was
there before the named cells were added. The setup page lists it last anyway;
the dropdown order and the id order are independent.
`CONFIG_TANENBASE_LC_DEFAULT_PROFILE` picks what a freshly flashed unit runs
until the setup page stores something.

## Why an uncharacterised cell ships with gain 0

`k_c` is a property of the **cell plus its mount**, not of the cell model. The
H40A's datasheet temperature effect on zero (±0.014 %FS/10 K → 2.1 g/K) is
**8× smaller** than the 17.7 g/K the assembled frame actually shows: most of the
drift is the mount, not the element.

So a plausible-looking number copied from a datasheet — or from a different
cell of the same class — is not a conservative default. If it has the wrong
sign or magnitude the correction *adds* error, silently, and it looks like the
cell is bad. Uncorrected is the honest starting point; anything else has to be
measured.

## Characterising a cell from a production week

The reference sweep was a bench run with a separate logger. You do not need one:
a station already reports raw weight and probe temperature, which is the whole
input to the fit. This is the route planned for the SBS-PF-150.

1. **Put the cell on a profile with gain 0** — its own named profile, or
   generic. This matters: the fit needs the **uncorrected** weight. A run
   logged with a correction already active has to be inverted first
   ([`H40A-C3-0150/analysis/field_validation.py`](H40A-C3-0150/analysis/field_validation.py)
   does that, but it is avoidable work).
2. **Put a dead weight on it and leave it alone.** 20–25 kg of something that
   does not evaporate, gain moisture or get walked on. No colony: a hive's real
   mass change is exactly the signal you are trying to separate the drift from.
   Outdoors, in the frame it will actually live in — the mount is most of what
   you are measuring.
3. **Shorten the uplink interval for the week.** At the 4 h heartbeat default a
   week gives ~42 points, and the daily cycle is what carries the information.
   Set *Senden alle* to 15–30 minutes on the setup page (*Einstellungen*) for
   the duration; 300–650 samples is the range the reference fit worked in.
   Put it back afterwards — that cadence costs battery (see
   [`../POWER_BUDGET.md`](../POWER_BUDGET.md)).
4. **Collect a full week including one clear hot day and one cold night.** The
   fit is driven by the temperature *span*: the reference run got 17.5 K.
   Rain is not fatal but it is a confounder — wood and concrete absorb
   hundreds of grams and give it back over days (H40A report §9).
5. **Export the uplinks.** Any sink works (TTN Storage Integration, BEEP, your
   own MQTT log). The decoder in [`../../ttndecoder/`](../../ttndecoder/) gives
   `weight_kg` / `Gewicht` in kg and `t` / `TempOut` in °C. Reshape to the
   semicolon CSV the analysis script expects:

   ```
   time;T_C;W_raw_kg
   2026-08-10T00:03:17Z;23.0;22.11
   ```

6. **Fit it.** Point
   [`H40A-C3-0150/analysis/tempcomp_analysis.py`](H40A-C3-0150/analysis/tempcomp_analysis.py)
   at the CSV. It reports `k`, sweeps `τ`, and prints the residual σ with and
   without the correction. Uneven sampling is fine — the filter uses the real
   `Δt` between rows — but a long gap re-seeds nothing, so check for holes.
7. **Sanity-check before shipping it.** `k_c = -k`. If σ does not improve by a
   clear multiple, or the sign differs from the reference cell's, something
   else moved during the week — do not enter that number. Hold-out validation
   (§3 of the H40A report) is the cheap way to tell a fit from a coincidence.
8. **Enter it as “own measured values”** (g/K and minutes), re-tare, and watch
   the dead weight for a few more days before trusting it.

## Adding a profile

1. **Measure.** Constant known load, 40 h minimum, one full daily temperature
   cycle, logging raw weight + probe temperature — on the bench, or from a
   production week as above. Method, pitfalls and the pass criteria are in
   [`H40A-C3-0150/README.md`](H40A-C3-0150/README.md) §9.
2. **Fit.** `python H40A-C3-0150/analysis/tempcomp_analysis.py` against your CSV
   gives `k`, `τ` and the residual σ. Sanity-check it on a hold-out window (§3)
   — a fit that only works on its own data is not a profile.
3. **Replay it on the firmware arithmetic** before believing it:
   `gcc -O2 -std=c99 -o host_test H40A-C3-0150/firmware/host_test.c H40A-C3-0150/firmware/tanen_tempcomp.c -lm`.
   The integer implementation has to land on the Python model's numbers.
4. **Enter it as “own measured values”** on the setup page (gain in g/K, lag in
   minutes) and let it run on a dead weight for a few days before trusting it.
5. Only then add it to `lc_profiles[]` in
   [`../../src/config/config.c`](../../src/config/config.c), with a directory
   here holding the data and the report.

One caveat that saves a re-measurement: `τ` describes the lag between the
temperature probe and the cell body, so it belongs to the **frame and the probe
placement**, not to the cell. Every characterised profile so far carries the
same 1500 s, and the optimum is broad — 15–40 min all land within 1 g. It is
`k_c` that needs the work.

## Changing the profile does not invalidate the tare

The correction is `k_c · (T_eff − T_ref)`, which is zero at `T_ref` for **any**
gain. `T_ref` is the temperature captured at the last tare, so switching
profiles leaves the zero where it was — no re-tare needed, only a re-tare after
mechanical changes.
