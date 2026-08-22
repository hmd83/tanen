#!/usr/bin/env python3
"""TANEN BASE — field validation of the deployed temperature correction.

Reads a log of COMPENSATED weight + probe temperature taken with the firmware
correction active, reconstructs T_eff with the firmware's exact fixed-point
filter, inverts the correction to recover the raw weight, and compares the two
on the same samples.

    python analysis/field_validation.py data/Loadcell_B_test2.csv [--svg out.svg]

Stdlib only, on purpose: this has to run on the same machine that flashes the
node, without a scientific Python stack.

SPDX-License-Identifier: MIT
"""
import argparse
import csv
import datetime as dt
import math

# Must match src/features/measurement/tempcomp.c / Kconfig.
GAIN_MG_PER_K = 17734
TAU_S = 1500
T_REF_MDEG = 25000  # only offsets the reconstructed raw series by a constant

UTC = dt.timezone.utc
# End of the rain-free window in the reference run. Set to None to skip
# segmentation and analyse the whole series as one block.
DRY_END = dt.datetime(2026, 8, 18, 17, 0, tzinfo=UTC)
DRY_DOWNS = [("2026-08-19T00:00", "2026-08-20T04:30"),
             ("2026-08-20T13:00", "2026-08-21T09:30")]


# ---------------------------------------------------------------- firmware maths

def c_div(a, b):
    """C integer division: truncates toward zero, unlike Python's floor."""
    return int(a / b) if (a < 0) != (b < 0) else a // b


def alpha_q16(dt_s, tau_s):
    den = dt_s + tau_s
    return min((dt_s << 16) // den, 1 << 16) if den else 1 << 16


def reconstruct(rows):
    """(time, T, W_corr_g) -> (time, T, W_corr_g, T_eff_C, W_raw_g)."""
    out, t_eff, prev = [], None, None
    for t, temp_c, w_kg in rows:
        t_mdeg = int(round(temp_c * 1000))
        if t_eff is None:
            t_eff = t_mdeg
        else:
            a = alpha_q16(int((t - prev).total_seconds()), TAU_S)
            t_eff += (t_mdeg - t_eff) * a >> 16
        prev = t
        corr_g = c_div(GAIN_MG_PER_K * (t_eff - T_REF_MDEG), 1000) / 1000.0
        w_g = w_kg * 1000.0
        out.append((t, temp_c, w_g, t_eff / 1000.0, w_g - corr_g))
    return out


# ---------------------------------------------------------------- statistics

def ols(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    slope = sxy / sxx if sxx else float("nan")
    r2 = (sxy * sxy / (sxx * syy)) if sxx and syy else float("nan")
    return slope, my - slope * mx, r2


def ols2(x1, x2, y):
    """y = a + b*x1 + c*x2 — separates a drying trend from the thermal term."""
    n = len(y)
    m1, m2, my = sum(x1) / n, sum(x2) / n, sum(y) / n
    a1 = [v - m1 for v in x1]
    a2 = [v - m2 for v in x2]
    ay = [v - my for v in y]
    s11 = sum(v * v for v in a1)
    s22 = sum(v * v for v in a2)
    s12 = sum(p * q for p, q in zip(a1, a2))
    s1y = sum(p * q for p, q in zip(a1, ay))
    s2y = sum(p * q for p, q in zip(a2, ay))
    det = s11 * s22 - s12 * s12
    return (s1y * s22 - s2y * s12) / det, (s2y * s11 - s1y * s12) / det


def stats(v):
    n = len(v)
    m = sum(v) / n
    return m, math.sqrt(sum((x - m) ** 2 for x in v) / n), max(v) - min(v)


def load(path):
    rows = []
    with open(path) as f:
        r = csv.reader(f, delimiter=";")
        next(r)
        for line in r:
            if len(line) >= 3:
                rows.append((dt.datetime.fromisoformat(line[0].replace("Z", "+00:00")),
                             float(line[1]), float(line[2])))
    return rows


def segment(data, start, end):
    return [d for d in data if start <= d[0] < end]


# ---------------------------------------------------------------- reporting

def report(name, seg):
    if len(seg) < 10:
        return
    T = [d[1] for d in seg]
    teff, wc, wr = [d[3] for d in seg], [d[2] for d in seg], [d[4] for d in seg]
    _, sdc, p2pc = stats(wc)
    _, sdr, p2pr = stats(wr)
    slc, _, r2c = ols(teff, wc)
    slr, _, r2r = ols(teff, wr)
    hours = (seg[-1][0] - seg[0][0]).total_seconds() / 3600
    print(f"\n=== {name} ===")
    print(f"  {seg[0][0]:%m-%d %H:%M} -> {seg[-1][0]:%m-%d %H:%M}  "
          f"({hours:.1f} h, n={len(seg)}), T {min(T)}..{max(T)} C")
    print(f"  compensated  : sigma={sdc:6.1f} g  p2p={p2pc:5.0f} g  "
          f"slope={slc:+7.2f} g/K  r2={r2c:.3f}")
    print(f"  uncompensated: sigma={sdr:6.1f} g  p2p={p2pr:5.0f} g  "
          f"slope={slr:+7.2f} g/K  r2={r2r:.3f}")
    if sdc:
        print(f"  improvement  : sigma {sdr/sdc:.1f}x  p2p {p2pr/p2pc:.1f}x")


def events(data, window_min=60, thresh_g=50):
    """Rapid sustained moves + the coefficient needed to explain them thermally.

    A rain event needs a coefficient far outside the model; a genuine thermal
    error would land near it.
    """
    print(f"\n=== moves >{thresh_g} g in {window_min} min ===")
    found, i = [], 0
    while i < len(data):
        t0, _, w0, te0, _ = data[i]
        j = i
        while j < len(data) and (data[j][0] - t0).total_seconds() <= window_min * 60:
            j += 1
        j = min(j, len(data) - 1)
        if abs(data[j][2] - w0) >= thresh_g:
            found.append((t0, data[j][0], data[j][2] - w0, data[j][3] - te0))
            i = j
        else:
            i += 1
    merged = []
    for e in found:
        if merged and (e[0] - merged[-1][1]).total_seconds() <= 3600:
            m = merged[-1]
            merged[-1] = (m[0], e[1], m[2] + e[2], m[3] + e[3])
        else:
            merged.append(e)
    for t0, t1, dw, dte in merged:
        need = dw / dte if dte else float("inf")
        print(f"  {t0:%m-%d %H:%M} -> {t1:%m-%d %H:%M}  dW={dw:+6.0f} g  "
              f"dT_eff={dte:+5.2f} K  needs {need:+8.1f} g/K "
              f"(model {-GAIN_MG_PER_K/1000:+.1f})")
    return merged


def drydown(data, start, end):
    """Separate evaporation from any residual thermal term."""
    seg = segment(data, start, end)
    if len(seg) < 30:
        return
    t0 = seg[0][0]
    hrs = [(d[0] - t0).total_seconds() / 3600 for d in seg]
    teff = [d[3] for d in seg]
    T = [d[1] for d in seg]
    dry_c, th_c = ols2(hrs, teff, [d[2] for d in seg])
    dry_r, th_r = ols2(hrs, teff, [d[4] for d in seg])
    print(f"\n=== dry-down {seg[0][0]:%m-%d %H:%M} -> {seg[-1][0]:%m-%d %H:%M} "
          f"(T {min(T)}..{max(T)} C, n={len(seg)}) ===")
    print(f"  compensated  : drying {dry_c:+6.1f} g/h   thermal {th_c:+7.2f} g/K")
    print(f"  uncompensated: drying {dry_r:+6.1f} g/h   thermal {th_r:+7.2f} g/K")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--svg", help="also render the figure to this path")
    args = ap.parse_args()

    data = reconstruct(load(args.csv))
    steps = sorted((data[i + 1][0] - data[i][0]).total_seconds()
                   for i in range(len(data) - 1))
    print(f"n={len(data)}  {data[0][0]} -> {data[-1][0]}")
    print(f"sample interval: median {steps[len(steps)//2]:.0f} s, max {steps[-1]:.0f} s")

    if DRY_END and data[0][0] < DRY_END < data[-1][0]:
        report("DRY WINDOW (pre-rain)", segment(data, data[0][0], DRY_END))
        report("WET (rain + dry-down)",
               segment(data, DRY_END, data[-1][0] + dt.timedelta(seconds=1)))
    report("WHOLE SERIES", data)
    events(data)
    for a, b in DRY_DOWNS:
        drydown(data, dt.datetime.fromisoformat(a).replace(tzinfo=UTC),
                dt.datetime.fromisoformat(b).replace(tzinfo=UTC))

    if args.svg:
        try:
            from make_field_figure import render
        except ImportError:
            print("\nmake_field_figure.py not found next to this script")
        else:
            render(data, args.svg)
            print("\nwrote", args.svg)


if __name__ == "__main__":
    main()
