#!/usr/bin/env python3
"""
TANEN BASE - temperature drift characterisation of a Steinberg SBS-PF-150,
from a production week (uplinked weight + probe temperature, gain 0).

Regenerates every number and figure in ../README.md from
../data/SBS-PF-150.csv, and writes ../data/SBS-PF-150_compensated.csv.

    pip install pandas numpy matplotlib
    python tempcomp_analysis.py

Differences from the H40A bench script (../../H40A-C3-0150/analysis/):
  - the lag filter uses each row's real dt, not the median: an uplink log
    has holes (dropped uplinks) that a bench logger does not;
  - the fit runs on a window, not the whole file: the log starts unloaded
    and ends in a non-thermal event, both shown in fig. 1 and 8;
  - the deployed constants are replayed through the firmware's integer
    maths (src/features/measurement/tempcomp.c), which uses the RC form of
    alpha. At this 14 min cadence that differs visibly from exp().

SPDX-License-Identifier: MIT
"""
import os
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "data", "SBS-PF-150.csv")
OUT_CSV = os.path.join(HERE, "..", "data", "SBS-PF-150_compensated.csv")
FIG = os.path.join(HERE, "..", "figures")
os.makedirs(FIG, exist_ok=True)

TAU_MIN = 25.0     # same lag as every other profile; the optimum is flat (fig. 3)
T_REF_C = 25.0     # reference temperature the correction is anchored to
FS_KG = 150.0      # Emax of the SBS-PF-150

# Fit window: first loaded sample -> before 10-01, a flat-temperature day
# that sits +42 g off the model (rain). Everything
# outside it is plotted but not fitted.
FIT_START = pd.Timestamp("2026-09-26T05:57:00Z")
FIT_END = pd.Timestamp("2026-10-01T00:00:00Z")
EVENT_START = pd.Timestamp("2026-10-01T15:00:00Z")   # the 15 °C excursion

plt.rcParams.update({
    "figure.facecolor": "white", "axes.facecolor": "white",
    "savefig.facecolor": "white", "font.size": 10,
    "axes.titlesize": 11, "axes.titleweight": "bold", "axes.labelsize": 10,
    "axes.grid": True, "grid.alpha": 0.25, "grid.linewidth": 0.6,
    "axes.spines.top": False, "axes.spines.right": False,
    "legend.frameon": False, "figure.dpi": 140, "savefig.bbox": "tight",
})
CW, CT, CG, CP, CO = "#1f6feb", "#d1242f", "#1a7f37", "#8250df", "#bc4c00"


def lagfilt(temp, tau_min, dt_s):
    """First-order low-pass, probe -> cell body. dt_s is per row."""
    a = 1 - np.exp(-(dt_s / 60.0) / tau_min)
    out = np.empty(len(temp))
    out[0] = temp[0]
    for i in range(1, len(temp)):
        out[i] = out[i - 1] + a[i] * (temp[i] - out[i - 1])
    return out


def firmware_replay(temp_c, w_kg, dt_s, gain_mg_per_k, tau_s, t_ref_mdeg):
    """Bit-exact copy of tempcomp.c: Q16 RC alpha, C truncating division."""
    t_eff, out = None, []
    for t, w, d in zip(temp_c, w_kg, dt_s):
        t_mdeg = int(round(t * 1000))
        if t_eff is None:
            t_eff = t_mdeg
        else:
            a = min((int(d) << 16) // (int(d) + tau_s), 1 << 16)
            t_eff += ((t_mdeg - t_eff) * a) >> 16
        num = gain_mg_per_k * (t_eff - t_ref_mdeg)
        corr = int(num / 1000)  # truncate toward zero, as C does
        out.append(int(round(w * 1e6)) + corr)
    return np.array(out) / 1e6


def load():
    df = pd.read_csv(DATA, sep=";", quotechar='"')
    df.columns = ["time", "T", "W"]
    df["time"] = pd.to_datetime(df["time"])
    df = df.dropna().reset_index(drop=True)
    df["dt"] = df.time.diff().dt.total_seconds().fillna(0.0)
    return df


def fit(temp_eff, weight):
    X = np.column_stack([np.ones(len(temp_eff)), temp_eff])
    coef, *_ = np.linalg.lstsq(X, weight, rcond=None)
    resid = weight - X @ coef
    n, p = len(weight), 2
    se = np.sqrt((resid @ resid / (n - p)) * np.linalg.inv(X.T @ X)[1, 1])
    return coef, resid, se


def main():
    full = load()
    fh = (full.time - FIT_START).dt.total_seconds().values / 3600.0
    sel = (full.time >= FIT_START) & (full.time < FIT_END)
    df = full[sel].reset_index(drop=True)
    dt_s = df.dt.values.copy(); dt_s[0] = 0.0
    h = (df.time - df.time[0]).dt.total_seconds().values / 3600.0
    T, W = df["T"].values, df.W.values
    Te = lagfilt(T, TAU_MIN, dt_s)
    coef, res, se = fit(Te, W)
    k = coef[1]
    r = np.corrcoef(Te, W)[0, 1]
    noise = np.std(np.diff(W)) / np.sqrt(2) * 1000
    gaps = full.dt[full.dt > 1.5 * full.dt.median()]

    print(f"file: n={len(full)}  median dt={full.dt[1:].median():.0f}s  "
          f"gaps={len(gaps)} (longest {gaps.max():.0f}s)")
    print(f"fit window {df.time.iloc[0]} .. {df.time.iloc[-1]}  "
          f"n={len(df)}  span={h.max():.1f} h")
    print(f"T {T.min()}..{T.max()} C   W {W.min()}..{W.max()} kg")
    print(f"k   = {k*1000:+.3f} g/K  (95% CI +/- {1.96*se*1000:.3f})")
    print(f"r2  = {r**2:.4f}   r2 vs instantaneous T = "
          f"{np.corrcoef(T, W)[0, 1]**2:.4f}")
    print(f"noise floor (sample-to-sample) = {noise:.1f} g")
    print(f"raw  sigma = {W.std()*1000:.2f} g   p2p = {np.ptp(W)*1000:.0f} g   "
          f"P95 = {np.percentile(np.abs(W - W.mean()), 95)*1000:.1f} g")
    print(f"comp sigma = {res.std()*1000:.2f} g   p2p = {np.ptp(res)*1000:.0f} g   "
          f"P95 = {np.percentile(np.abs(res), 95)*1000:.1f} g")
    print(f"     %FS  raw {W.std()/FS_KG*100:.4f}  comp {res.std()/FS_KG*100:.4f}")

    # sensitivity of k to the window ----------------------------------------
    for name, a, b in (("+ 10-01 to 15:00", FIT_START, EVENT_START),
                       ("from 09-27 00:00", pd.Timestamp("2026-09-27T00:00Z"), FIT_END)):
        s = full[(full.time >= a) & (full.time < b)].reset_index(drop=True)
        d = s.dt.values.copy(); d[0] = 0
        c, rr, _ = fit(lagfilt(s["T"].values, TAU_MIN, d), s.W.values)
        print(f"window {name:18s}: k = {c[1]*1000:+.2f} g/K  sigma = {rr.std()*1000:.1f} g")

    # time trend, diagnostic only (never a correction - see README §6)
    X = np.column_stack([np.ones(len(Te)), Te, h / 24])
    ct, *_ = np.linalg.lstsq(X, W, rcond=None)
    print(f"with trend term: k = {ct[1]*1000:+.2f} g/K, trend = {ct[2]*1000:+.1f} g/day, "
          f"sigma = {(W - X @ ct).std()*1000:.1f} g")

    # 01 raw overview --------------------------------------------------------
    fig, ax = plt.subplots(figsize=(11, 4.2))
    ax.plot(fh[1:], full.W.values[1:], color=CW, lw=1.2)
    ax.set_xlabel("Elapsed time [h]"); ax.set_ylabel("Weight [kg]", color=CW)
    ax.tick_params(axis="y", labelcolor=CW); ax.set_ylim(9.70, 10.75)
    ax2 = ax.twinx(); ax2.plot(fh, full["T"].values, color=CT, lw=1.2, alpha=.85)
    ax2.set_ylabel("Temperature [°C]", color=CT)
    ax2.tick_params(axis="y", labelcolor=CT); ax2.grid(False)
    ax2.spines["top"].set_visible(False)
    x_end = (FIT_END - FIT_START).total_seconds() / 3600
    x_ev = (EVENT_START - FIT_START).total_seconds() / 3600
    ax.axvspan(x_end, x_ev, color="#d0d7de", alpha=.35, lw=0)
    ax.axvspan(x_ev, fh.max(), color=CO, alpha=.10, lw=0)
    ax.text(x_end + 0.5, 10.72, "10-01", fontsize=8.5, color="#57606a", va="top")
    ax.text(x_ev + 0.5, 10.72, "event", fontsize=8.5, color=CO, va="top")
    ax.set_title("Steinberg SBS-PF-150 — raw weight vs. temperature  "
                 f"(static {W.mean():.1f} kg reference load, {fh.max():.0f} h)")
    ax.text(0.015, 0.06,
            f"Fit window (unshaded, {h.max():.0f} h): weight swings "
            f"{np.ptp(W)*1000:.0f} g p-p with a {np.ptp(T):.1f} K cycle.\n"
            "Shaded: excluded from the fit — see §4.",
            transform=ax.transAxes, ha="left", va="bottom", fontsize=9,
            color="#57606a",
            bbox=dict(fc="#f6f8fa", ec="#d0d7de", boxstyle="round,pad=0.5"))
    fig.savefig(f"{FIG}/01_raw_overview.png"); plt.close(fig)

    # 02 hysteresis is lag ---------------------------------------------------
    fig, axs = plt.subplots(1, 2, figsize=(11, 4.4), sharey=True)
    for a, xv, ttl in (
        (axs[0], T, "Instantaneous sensor temperature  T(t)"),
        (axs[1], Te, f"Lag-filtered temperature  T_eff(t),  τ = {TAU_MIN:.0f} min")
    ):
        d = pd.Series(xv).diff().rolling(5).mean().values
        a.scatter(xv[d > 0.01], (W[d > 0.01] - W.mean()) * 1000,
                  s=9, alpha=.55, color=CO, label="T rising")
        a.scatter(xv[d < -0.01], (W[d < -0.01] - W.mean()) * 1000,
                  s=9, alpha=.55, color=CG, label="T falling")
        sl, ic = np.polyfit(xv, W, 1)
        rr = np.corrcoef(xv, W)[0, 1]
        xs = np.linspace(xv.min(), xv.max(), 10)
        a.plot(xs, (sl * xs + ic - W.mean()) * 1000, "k--", lw=1.6,
               label=f"{sl*1000:.2f} g/K,  r² = {rr**2:.3f}")
        rs = (W - (sl * xv + ic)) * 1000
        print(f"loop {ttl[:14]}: rising {rs[d > 0.01].mean():+.1f} g, "
              f"falling {rs[d < -0.01].mean():+.1f} g")
        a.set_xlabel("Temperature [°C]"); a.set_title(ttl)
        a.legend(fontsize=8.5, loc="upper right")
    axs[0].set_ylabel("Weight deviation [g]")
    fig.suptitle("Probe-to-cell lag is small on this frame — "
                 "the filter barely changes the fit",
                 fontsize=11.5, fontweight="bold", y=1.01)
    fig.tight_layout(); fig.savefig(f"{FIG}/02_hysteresis_lag.png"); plt.close(fig)

    # 03 tau sweep -----------------------------------------------------------
    taus = np.arange(1, 121)
    sig = np.array([fit(lagfilt(T, t, dt_s), W)[1].std() * 1000 for t in taus])
    s0 = fit(T, W)[1].std() * 1000
    s25 = sig[int(TAU_MIN) - 1]
    print(f"tau sweep: no filter {s0:.2f} g, best tau={taus[sig.argmin()]} min "
          f"{sig.min():.2f} g, tau 25 {s25:.2f} g, "
          f"within 1 g: {taus[sig < sig.min() + 1].min()}-{taus[sig < sig.min() + 1].max()} min")
    fig, ax = plt.subplots(figsize=(7, 3.8))
    ax.plot(taus, sig, color=CP, lw=1.8)
    ax.axvline(TAU_MIN, color=CT, ls="--", lw=1.2)
    ax.plot(TAU_MIN, s25, "o", color=CT, ms=7)
    ax.annotate(f"τ = {TAU_MIN:.0f} min\nσ = {s25:.1f} g",
                (TAU_MIN, s25), xytext=(TAU_MIN + 14, s25 + 2),
                fontsize=9, color=CT)
    ax.axhline(s0, color="#8c959f", ls=":", lw=1)
    ax.text(118, s0 + 0.15, f"no lag filter: {s0:.1f} g", va="bottom",
            ha="right", fontsize=8.5, color="#57606a")
    ax.set_xlabel("Thermal time constant τ [min]")
    ax.set_ylabel("Residual σ [g]")
    ax.set_title("Optimisation of the thermal lag constant")
    fig.savefig(f"{FIG}/03_tau_sweep.png"); plt.close(fig)

    # 04 before / after ------------------------------------------------------
    gain = W.std() / res.std()
    fig, axs = plt.subplots(2, 1, figsize=(11, 6.2), sharex=True)
    a = axs[0]
    a.plot(h, (W - W.mean()) * 1000, color="#8c959f", lw=1.2,
           label=f"Uncompensated  (σ = {W.std()*1000:.0f} g, "
                 f"p-p = {np.ptp(W)*1000:.0f} g)")
    a.axhline(0, color="k", lw=.8); a.set_ylabel("Deviation [g]")
    a.legend(fontsize=9, loc="upper right"); a.set_ylim(-140, 140)
    a.set_title("Before compensation")
    a = axs[1]
    a.plot(h, res * 1000, color=CW, lw=1.2,
           label=f"Compensated  (σ = {res.std()*1000:.1f} g, "
                 f"p-p = {np.ptp(res)*1000:.0f} g)")
    a.fill_between(h, -res.std() * 1000, res.std() * 1000,
                   color=CW, alpha=.13, label="±1σ")
    a.axhline(0, color="k", lw=.8)
    a.axhspan(-noise, noise, color=CG, alpha=.13,
              label=f"ADC/quantisation noise floor (±{noise:.1f} g)")
    a.set_ylabel("Residual [g]"); a.set_xlabel("Elapsed time [h]")
    a.legend(fontsize=9, loc="upper right", ncol=3); a.set_ylim(-140, 140)
    a.set_title(f"After compensation:  W_corr = W_raw + {abs(k)*1000:.2f} g/K"
                f" · (T_eff − {T_REF_C:.0f} °C)   →  {gain:.1f}× improvement")
    fig.tight_layout(); fig.savefig(f"{FIG}/04_before_after.png"); plt.close(fig)

    # 05 error distribution --------------------------------------------------
    fig, axs = plt.subplots(1, 2, figsize=(11, 3.9))
    bins = np.arange(-120, 125, 10)
    axs[0].hist((W - W.mean()) * 1000, bins=bins, color="#8c959f",
                alpha=.75, label="Uncompensated")
    axs[0].hist(res * 1000, bins=bins, color=CW, alpha=.85, label="Compensated")
    axs[0].set_xlabel("Error [g]"); axs[0].set_ylabel("Samples")
    axs[0].legend(fontsize=9); axs[0].set_title("Error distribution")
    lv = [50, 68, 90, 95, 99]
    u = np.percentile(np.abs(W - W.mean()) * 1000, lv)
    v = np.percentile(np.abs(res) * 1000, lv)
    x = np.arange(len(lv)); wd = 0.36
    axs[1].bar(x - wd / 2, u, wd, color="#8c959f", label="Uncompensated")
    axs[1].bar(x + wd / 2, v, wd, color=CW, label="Compensated")
    for i, (uu, vv) in enumerate(zip(u, v)):
        axs[1].text(i - wd / 2, uu + 2, f"{uu:.0f}", ha="center",
                    fontsize=8, color="#57606a")
        axs[1].text(i + wd / 2, vv + 2, f"{vv:.0f}", ha="center",
                    fontsize=8, color=CW)
    axs[1].set_xticks(x); axs[1].set_xticklabels([f"P{l}" for l in lv])
    axs[1].set_ylabel("|error| [g]"); axs[1].legend(fontsize=9)
    axs[1].set_title("Absolute error percentiles")
    fig.tight_layout(); fig.savefig(f"{FIG}/05_error_distribution.png"); plt.close(fig)

    # 06 hold-out validation -------------------------------------------------
    split_h = round(h.max() / 2)
    tr, va = h < split_h, h >= split_h
    ctr, _, _ = fit(Te[tr], W[tr])
    err = (W - (ctr[0] + ctr[1] * Te)) * 1000
    trend = np.polyfit(h / 24, err, 1)
    fig, ax = plt.subplots(figsize=(11, 4.0))
    ax.plot(h[tr], err[tr], color=CW, lw=1.2,
            label=f"Training window (0–{split_h} h): k = {ctr[1]*1000:.2f} g/K")
    ax.plot(h[va], err[va], color=CO, lw=1.2,
            label=f"Hold-out ({split_h}–{h.max():.0f} h): σ = {err[va].std():.1f} g, "
                  f"bias = {err[va].mean():+.1f} g")
    ax.axvline(split_h, color="k", ls="--", lw=1); ax.axhline(0, color="k", lw=.8)
    ax.plot(h, np.polyval(trend, h / 24), color=CP, ls=":", lw=1.8,
            label=f"Residual time trend: {trend[0]:+.1f} g/day")
    ax.set_xlabel("Elapsed time [h]"); ax.set_ylabel("Prediction error [g]")
    ax.set_title(f"Hold-out validation — coefficient fitted on the first {split_h} h, "
                 "applied blind to the rest")
    ax.set_ylim(-60, 105); ax.legend(fontsize=9, loc="upper left")
    fig.savefig(f"{FIG}/06_validation.png"); plt.close(fig)
    print(f"k_train = {ctr[1]*1000:+.3f} g/K (0-{split_h} h)   "
          f"hold-out sigma = {err[va].std():.1f} g, bias = {err[va].mean():+.1f} g")
    print(f"residual time trend = {trend[0]:+.1f} g/day")

    # 07 residual by hour of day --------------------------------------------
    hr = df.time.dt.hour.values
    rm = np.array([res[hr == i].mean() * 1000 for i in range(24)])
    rse = np.array([res[hr == i].std() * 1000 / np.sqrt((hr == i).sum())
                    for i in range(24)])
    tm = np.array([T[hr == i].mean() for i in range(24)])
    dtm = np.gradient(tm)
    print("residual by UTC hour:", " ".join(f"{v:+.0f}" for v in rm))
    print(f"  min {rm.min():+.1f} g at {rm.argmin():02d} UTC, "
          f"max {rm.max():+.1f} g at {rm.argmax():02d} UTC; "
          f"corr(resid, dT/dt) = {np.corrcoef(rm, dtm)[0, 1]:+.2f}")
    fig, ax = plt.subplots(figsize=(11, 3.8))
    ax.bar(np.arange(24), rm, yerr=rse, color=CW, alpha=.8, capsize=2,
           error_kw=dict(lw=.8, ecolor="#57606a"), label="Mean residual ± SE")
    ax.axhline(0, color="k", lw=.8)
    ax.set_xlabel("Hour of day [UTC]  (local CEST = UTC+2)")
    ax.set_ylabel("Compensated residual [g]"); ax.set_xticks(range(0, 24, 2))
    ax2 = ax.twinx(); ax2.plot(np.arange(24), tm, color=CT, lw=1.6, marker="o",
                               ms=3, label="Mean probe temperature")
    ax2.set_ylabel("Temperature [°C]", color=CT); ax2.grid(False)
    ax2.tick_params(axis="y", labelcolor=CT); ax2.spines["top"].set_visible(False)
    ax.set_title("What the correction leaves: a midday dip the probe temperature "
                 "does not explain")
    h1, l1 = ax.get_legend_handles_labels(); h2, l2 = ax2.get_legend_handles_labels()
    ax.legend(h1 + h2, l1 + l2, fontsize=9, loc="lower left")
    fig.savefig(f"{FIG}/07_diurnal_residual.png"); plt.close(fig)

    # 08 excluded tail vs model ---------------------------------------------
    tail = full[full.time >= FIT_START].reset_index(drop=True)
    td = tail.dt.values.copy(); td[0] = 0
    tTe = lagfilt(tail["T"].values, TAU_MIN, td)
    pred = coef[0] + k * tTe
    dev = (tail.W.values - pred) * 1000
    th = (tail.time - FIT_START).dt.total_seconds().values / 3600
    wet = (tail.time >= FIT_END) & (tail.time < EVENT_START)
    ev = tail.time >= EVENT_START
    print(f"wet day 10-01 00-15 UTC: mean deviation from model {dev[wet].mean():+.1f} g, "
          f"max {dev[wet].max():+.0f} g, T {tail['T'][wet].min()}..{tail['T'][wet].max()} C")
    print(f"event: peak {dev[ev].max():+.0f} g at {tail.time[ev][dev[ev].argmax() + ev.values.argmax()]}, "
          f"last {dev[ev][-1]:+.0f} g, T {tail['T'][ev].min()}..{tail['T'][ev].max()} C")
    last4 = tail[ev].tail(4)
    rate = np.diff(last4.W.values).mean() * 1000 / (last4.dt.values[1:].mean() / 3600)
    print(f"  last hour: {rate:+.0f} g/h")
    z = th >= (FIT_END - FIT_START).total_seconds() / 3600 - 24
    fig, ax = plt.subplots(figsize=(11, 4.0))
    ax.plot(th[z], tail.W.values[z], color=CW, lw=1.3, label="Measured weight")
    ax.plot(th[z], pred[z], color="#57606a", lw=1.3, ls="--",
            label=f"Model from the fit window (k = {k*1000:.2f} g/K)")
    ax.set_xlabel("Elapsed time [h]"); ax.set_ylabel("Weight [kg]")
    ax.axvspan(th[wet].min(), th[ev].min(), color="#d0d7de", alpha=.35, lw=0)
    ax.axvspan(th[ev].min(), th.max(), color=CO, alpha=.10, lw=0)
    ax2 = ax.twinx(); ax2.plot(th[z], tail["T"].values[z], color=CT, lw=1.1, alpha=.8,
                               label="Temperature")
    ax2.set_ylabel("Temperature [°C]", color=CT); ax2.grid(False)
    ax2.tick_params(axis="y", labelcolor=CT); ax2.spines["top"].set_visible(False)
    h1, l1 = ax.get_legend_handles_labels(); h2, l2 = ax2.get_legend_handles_labels()
    ax.legend(h1 + h2, l1 + l2, fontsize=9, loc="lower center")
    ax.set_title("Excluded tail — +0.6 kg up and 0.9 kg down at a flat 15 °C is not "
                 "thermal drift")
    fig.savefig(f"{FIG}/08_excluded_tail.png"); plt.close(fig)

    # firmware replay of the deployed constants ------------------------------
    gain_mg = int(round(-k * 1e6))
    wc = firmware_replay(T, W, dt_s, gain_mg, int(TAU_MIN * 60), int(T_REF_C * 1000))
    print(f"firmware replay gain={gain_mg} mg/K tau={int(TAU_MIN*60)} s: "
          f"mean={wc.mean():.4f} kg sigma={wc.std()*1000:.1f} g p2p={np.ptp(wc)*1000:.0f} g")

    # compensated CSV, same columns as the H40A one --------------------------
    wcorr = W + (-k) * (Te - T_REF_C)
    out = pd.DataFrame({
        "time": df.time.dt.strftime("%Y-%m-%dT%H:%M:%SZ"), "T_C": T, "W_raw_kg": W,
        "T_eff_C": Te.round(3), "W_corr_kg": wcorr.round(4),
        "residual_g": (res * 1000).round(1)})
    out.to_csv(OUT_CSV, sep=";", index=False)
    print(f"figures written to {os.path.normpath(FIG)}")


if __name__ == "__main__":
    main()
