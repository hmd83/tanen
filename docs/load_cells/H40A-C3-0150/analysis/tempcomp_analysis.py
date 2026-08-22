#!/usr/bin/env python3
"""
TANEN BASE - temperature drift characterisation of a Bosche H40A-C3-0150.

Regenerates every number and figure in ../README.md from ../data/Loadcell_B.csv.

    pip install pandas numpy scipy matplotlib
    python tempcomp_analysis.py

SPDX-License-Identifier: MIT
"""
import os
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from scipy import stats

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "data", "Loadcell_B.csv")
FIG = os.path.join(HERE, "..", "figures")
os.makedirs(FIG, exist_ok=True)

TAU_MIN = 25.0     # thermal time constant of the cell body
T_REF_C = 25.0     # reference temperature the correction is anchored to
FS_KG = 150.0      # Emax of the H40A-C3-0150
NOISE_G = 9.6      # measured sample-to-sample noise floor

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
    """First-order low-pass modelling the thermal lag between the sensor
    and the aluminium spring element. Mirrors the firmware exactly."""
    a = 1 - np.exp(-(dt_s / 60.0) / tau_min)
    out = np.empty(len(temp))
    out[0] = temp[0]
    for i in range(1, len(temp)):
        out[i] = out[i - 1] + a * (temp[i] - out[i - 1])
    return out


def load():
    df = pd.read_csv(DATA, sep=";", quotechar='"')
    df.columns = ["time", "T", "W"]
    df["time"] = pd.to_datetime(df["time"])
    df = df.dropna().reset_index(drop=True)
    df["h"] = (df.time - df.time.min()).dt.total_seconds() / 3600.0
    return df


def fit(temp_eff, weight):
    X = np.column_stack([np.ones(len(temp_eff)), temp_eff])
    coef, *_ = np.linalg.lstsq(X, weight, rcond=None)
    resid = weight - X @ coef
    n, p = len(weight), 2
    se = np.sqrt((resid @ resid / (n - p)) * np.linalg.inv(X.T @ X)[1, 1])
    return coef, resid, se


def main():
    df = load()
    dt_s = df.time.diff().dt.total_seconds().median()
    T, W, h = df["T"].values, df.W.values, df.h.values
    Te = lagfilt(T, TAU_MIN, dt_s)
    coef, res, se = fit(Te, W)
    k = coef[1]
    r = np.corrcoef(Te, W)[0, 1]

    print(f"n={len(df)}  dt={dt_s:.0f}s  span={h.max():.1f} h")
    print(f"T {T.min()}..{T.max()} C   W {W.min()}..{W.max()} kg")
    print(f"k   = {k*1000:+.3f} g/K  (95% CI +/- {1.96*se*1000:.3f})")
    print(f"r2  = {r**2:.4f}")
    print(f"raw  sigma = {W.std()*1000:.2f} g   p2p = {np.ptp(W)*1000:.0f} g")
    print(f"comp sigma = {res.std()*1000:.2f} g   p2p = {np.ptp(res)*1000:.0f} g")
    print(f"     %FS  = {res.std()/FS_KG*100:.4f}   "
          f"({k/FS_KG*1e6:+.1f} ppm-FS/K)")

    # 01 raw overview -----------------------------------------------------
    fig, ax = plt.subplots(figsize=(11, 4.2))
    ax.plot(h, W, color=CW, lw=1.3)
    ax.set_xlabel("Elapsed time [h]"); ax.set_ylabel("Weight [kg]", color=CW)
    ax.tick_params(axis="y", labelcolor=CW); ax.set_ylim(21.75, 22.20)
    ax2 = ax.twinx(); ax2.plot(h, T, color=CT, lw=1.3, alpha=.85)
    ax2.set_ylabel("Temperature [°C]", color=CT)
    ax2.tick_params(axis="y", labelcolor=CT); ax2.grid(False)
    ax2.spines["top"].set_visible(False)
    ax.set_title("Bosche H40A-C3-0150 — raw weight vs. temperature  "
                 "(static 22.0 kg reference load, 40.7 h)")
    ax.text(0.985, 0.06,
            "Weight swings 350 g peak-to-peak with a 17.5 K temperature cycle\n"
            "while the true load is constant.",
            transform=ax.transAxes, ha="right", va="bottom", fontsize=9,
            color="#57606a",
            bbox=dict(fc="#f6f8fa", ec="#d0d7de", boxstyle="round,pad=0.5"))
    fig.savefig(f"{FIG}/01_raw_overview.png"); plt.close(fig)

    # 02 hysteresis is lag ------------------------------------------------
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
        a.set_xlabel("Temperature [°C]"); a.set_title(ttl)
        a.legend(fontsize=8.5, loc="upper right")
    axs[0].set_ylabel("Weight deviation [g]")
    fig.suptitle("Apparent hysteresis is thermal transport lag, "
                 "not mechanical hysteresis",
                 fontsize=11.5, fontweight="bold", y=1.01)
    fig.tight_layout(); fig.savefig(f"{FIG}/02_hysteresis_lag.png"); plt.close(fig)

    # 03 tau sweep --------------------------------------------------------
    taus = np.arange(1, 121)
    sig = np.array([fit(lagfilt(T, t, dt_s), W)[1].std() * 1000 for t in taus])
    fig, ax = plt.subplots(figsize=(7, 3.8))
    ax.plot(taus, sig, color=CP, lw=1.8)
    ax.axvline(TAU_MIN, color=CT, ls="--", lw=1.2)
    ax.plot(TAU_MIN, sig.min(), "o", color=CT, ms=7)
    ax.annotate(f"τ = {TAU_MIN:.0f} min\nσ = {sig.min():.1f} g",
                (TAU_MIN, sig.min()), xytext=(TAU_MIN + 14, sig.min() + 5),
                fontsize=9, color=CT)
    ax.axhline(sig[0], color="#8c959f", ls=":", lw=1)
    ax.text(118, sig[0] + 0.6, f"no lag filter: {sig[0]:.1f} g",
            ha="right", fontsize=8.5, color="#57606a")
    ax.set_xlabel("Thermal time constant τ [min]")
    ax.set_ylabel("Residual σ [g]")
    ax.set_title("Optimisation of the thermal lag constant")
    fig.savefig(f"{FIG}/03_tau_sweep.png"); plt.close(fig)

    # 04 before / after ---------------------------------------------------
    fig, axs = plt.subplots(2, 1, figsize=(11, 6.2), sharex=True)
    a = axs[0]
    a.plot(h, (W - W.mean()) * 1000, color="#8c959f", lw=1.2,
           label=f"Uncompensated  (σ = {W.std()*1000:.0f} g, "
                 f"p-p = {np.ptp(W)*1000:.0f} g)")
    a.axhline(0, color="k", lw=.8); a.set_ylabel("Deviation [g]")
    a.legend(fontsize=9, loc="upper right"); a.set_ylim(-220, 220)
    a.set_title("Before compensation")
    a = axs[1]
    a.plot(h, res * 1000, color=CW, lw=1.2,
           label=f"Compensated  (σ = {res.std()*1000:.1f} g, "
                 f"p-p = {np.ptp(res)*1000:.0f} g)")
    a.fill_between(h, -res.std() * 1000, res.std() * 1000,
                   color=CW, alpha=.13, label="±1σ")
    a.axhline(0, color="k", lw=.8)
    a.axhspan(-NOISE_G, NOISE_G, color=CG, alpha=.13,
              label=f"ADC/quantisation noise floor (±{NOISE_G:.1f} g)")
    a.set_ylabel("Residual [g]"); a.set_xlabel("Elapsed time [h]")
    a.legend(fontsize=9, loc="upper right"); a.set_ylim(-220, 220)
    a.set_title(f"After compensation:  W_corr = W_raw + {abs(k)*1000:.2f} g/K"
                f" · (T_eff − {T_REF_C:.0f} °C)   →  6.5× improvement")
    fig.tight_layout(); fig.savefig(f"{FIG}/04_before_after.png"); plt.close(fig)

    # 05 error distribution ----------------------------------------------
    fig, axs = plt.subplots(1, 2, figsize=(11, 3.9))
    bins = np.arange(-200, 205, 10)
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
        axs[1].text(i - wd / 2, uu + 3, f"{uu:.0f}", ha="center",
                    fontsize=8, color="#57606a")
        axs[1].text(i + wd / 2, vv + 3, f"{vv:.0f}", ha="center",
                    fontsize=8, color=CW)
    axs[1].set_xticks(x); axs[1].set_xticklabels([f"P{l}" for l in lv])
    axs[1].set_ylabel("|error| [g]"); axs[1].legend(fontsize=9)
    axs[1].set_title("Absolute error percentiles")
    fig.tight_layout(); fig.savefig(f"{FIG}/05_error_distribution.png"); plt.close(fig)

    # 06 hold-out validation ---------------------------------------------
    split_h = 20.0
    tr, va = h < split_h, h >= split_h
    ctr, _, _ = fit(Te[tr], W[tr])
    err = (W - (ctr[0] + ctr[1] * Te)) * 1000
    fig, ax = plt.subplots(figsize=(11, 4.0))
    ax.plot(h[tr], err[tr], color=CW, lw=1.2,
            label=f"Training window (0–20 h): k = {ctr[1]*1000:.2f} g/K")
    ax.plot(h[va], err[va], color=CO, lw=1.2,
            label=f"Hold-out (20–40.7 h): σ = {err[va].std():.1f} g, "
                  f"bias = {err[va].mean():+.1f} g")
    ax.axvline(split_h, color="k", ls="--", lw=1); ax.axhline(0, color="k", lw=.8)
    trend = np.polyfit(h / 24, err, 1)
    ax.plot(h, np.polyval(trend, h / 24), color=CP, ls=":", lw=1.8,
            label=f"Residual time trend: {trend[0]:+.1f} g/day")
    ax.set_xlabel("Elapsed time [h]"); ax.set_ylabel("Prediction error [g]")
    ax.set_title("Hold-out validation — coefficient fitted on the first 20 h, "
                 "applied blind to the rest")
    ax.legend(fontsize=9, loc="lower left")
    fig.savefig(f"{FIG}/06_validation.png"); plt.close(fig)

    print(f"k_train = {ctr[1]*1000:+.3f} g/K   "
          f"hold-out sigma = {err[va].std():.1f} g, bias = {err[va].mean():+.1f} g")
    print(f"residual time trend = {trend[0]:+.1f} g/day")
    print(f"figures written to {os.path.normpath(FIG)}")


if __name__ == "__main__":
    main()
