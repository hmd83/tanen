#!/usr/bin/env python3
"""Render the field-validation figure (figures/07_field_validation.svg).

Panel A: 6 days of compensated weight + probe temperature, rain events shaded.
Panel B: weight vs filtered temperature over the dry window, raw vs compensated.

Stdlib only — no matplotlib, same constraint as field_validation.py.

    python analysis/make_field_figure.py data/Loadcell_B_test2.csv \
        figures/07_field_validation.svg

SPDX-License-Identifier: MIT
"""
import datetime as dt
import sys

from field_validation import DRY_END, load, ols, reconstruct, segment

UTC = dt.timezone.utc

W, H = 960, 720
INK, MUTED, GRID = "#1b1b1b", "#6b6b6b", "#e3e3e3"
CORR, RAW, TEMP, RAIN = "#1f6feb", "#d1483f", "#e8a33d", "#cfe3f5"

# Rain events, from the >50 g/h move detector in field_validation.py.
RAINS = [("2026-08-18T16:00", "2026-08-18T20:30"),
         ("2026-08-20T04:30", "2026-08-20T11:00"),
         ("2026-08-21T09:30", "2026-08-21T12:00"),
         ("2026-08-21T15:45", "2026-08-21T19:30")]


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def render(data, out_path):
    dry = segment(data, data[0][0], DRY_END)
    t0, t1 = data[0][0], data[-1][0]
    span = (t1 - t0).total_seconds()

    ax, ay, aw, ah = 70, 56, W - 140, 300
    wmin, wmax, tmin, tmax = 22.10, 22.85, 12.0, 36.0

    def px(t):
        return ax + aw * (t - t0).total_seconds() / span

    def pyw(w):
        return ay + ah * (1 - (w - wmin) / (wmax - wmin))

    def pyt(c):
        return ay + ah * (1 - (c - tmin) / (tmax - tmin))

    s = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
         f'width="{W}" height="{H}" font-family="Segoe UI,Helvetica,Arial,sans-serif">',
         f'<rect width="{W}" height="{H}" fill="#ffffff"/>',
         f'<text x="{ax}" y="26" font-size="16" font-weight="600" fill="{INK}">'
         'Field validation &#8212; 22.2 kg dead weight, correction active, 6 days</text>',
         f'<text x="{ax}" y="44" font-size="12" fill="{MUTED}">2026-08-16 to 08-21 '
         '&#183; dry window flat to +1.1 g/K &#183; everything after 08-18 is rain mass, not drift</text>']

    for a, b in RAINS:
        xa = px(dt.datetime.fromisoformat(a).replace(tzinfo=UTC))
        xb = px(dt.datetime.fromisoformat(b).replace(tzinfo=UTC))
        s.append(f'<rect x="{xa:.1f}" y="{ay}" width="{max(xb-xa,2):.1f}" height="{ah}" '
                 f'fill="{RAIN}" opacity="0.75"/>')
    xm = px(DRY_END)
    s.append(f'<line x1="{xm:.1f}" y1="{ay}" x2="{xm:.1f}" y2="{ay+ah}" stroke="{MUTED}" '
             f'stroke-width="1" stroke-dasharray="4 3"/>')
    s.append(f'<text x="{ax+8}" y="{ay+16}" font-size="11" fill="{MUTED}">'
             'dry window &#8212; 64.9 h, 19.5-34.0 &#176;C</text>')
    s.append(f'<text x="{xm+8:.1f}" y="{ay+16}" font-size="11" fill="{MUTED}">'
             'rain (shaded) + dry-down</text>')

    w = wmin
    while w <= wmax + 1e-9:
        y = pyw(w)
        s.append(f'<line x1="{ax}" y1="{y:.1f}" x2="{ax+aw}" y2="{y:.1f}" stroke="{GRID}"/>')
        s.append(f'<text x="{ax-8}" y="{y+4:.1f}" font-size="11" text-anchor="end" '
                 f'fill="{MUTED}">{w:.2f}</text>')
        w += 0.15
    s.append(f'<text x="{ax-46}" y="{ay+ah/2:.0f}" font-size="12" fill="{INK}" '
             f'transform="rotate(-90 {ax-46} {ay+ah/2:.0f})" text-anchor="middle">weight (kg)</text>')
    for c in range(15, 36, 5):
        s.append(f'<text x="{ax+aw+8}" y="{pyt(c)+4:.1f}" font-size="11" fill="{TEMP}">{c}&#176;</text>')
    s.append(f'<text x="{ax+aw+52}" y="{ay+ah/2:.0f}" font-size="12" fill="{TEMP}" '
             f'transform="rotate(90 {ax+aw+52} {ay+ah/2:.0f})" text-anchor="middle">'
             'temperature (&#176;C)</text>')

    d = t0.replace(hour=0, minute=0, second=0, microsecond=0) + dt.timedelta(days=1)
    while d < t1:
        x = px(d)
        s.append(f'<line x1="{x:.1f}" y1="{ay}" x2="{x:.1f}" y2="{ay+ah}" stroke="{GRID}"/>')
        s.append(f'<text x="{x:.1f}" y="{ay+ah+16}" font-size="11" text-anchor="middle" '
                 f'fill="{MUTED}">{d:%b %d}</text>')
        d += dt.timedelta(days=1)

    def path(pts):
        return "M" + " L".join(f"{x:.1f},{y:.1f}" for x, y in pts)

    s.append(f'<path d="{path([(px(p[0]), pyt(p[1])) for p in data])}" fill="none" '
             f'stroke="{TEMP}" stroke-width="1" opacity="0.85"/>')
    s.append(f'<path d="{path([(px(p[0]), pyw(p[2]/1000)) for p in data])}" fill="none" '
             f'stroke="{CORR}" stroke-width="1.6"/>')

    def ann(iso, text, dy=-10, anchor="middle"):
        t = dt.datetime.fromisoformat(iso).replace(tzinfo=UTC)
        near = min(data, key=lambda p: abs((p[0] - t).total_seconds()))
        s.append(f'<text x="{px(near[0]):.1f}" y="{pyw(near[2]/1000)+dy:.1f}" font-size="11" '
                 f'text-anchor="{anchor}" fill="{INK}">{esc(text)}</text>')

    ann("2026-08-20T09:00", "+310 g in 6 h", -14)
    ann("2026-08-21T11:30", "+220 g in 2 h, T_eff moved 0.4 K", -16, "end")
    s.append(f'<text x="{ax+aw-4}" y="{ay+ah-8}" font-size="11" text-anchor="end" '
             f'fill="{MUTED}">+506 g water retained at end</text>')

    bx, by, bw, bh = 70, 440, W - 140, 210
    teff = [p[3] for p in dry]
    tlo, thi = min(teff) - 0.5, max(teff) + 0.5
    mc = sum(p[2] for p in dry) / len(dry)
    mr = sum(p[4] for p in dry) / len(dry)
    dev_c = [p[2] - mc for p in dry]
    dev_r = [p[4] - mr for p in dry]
    lo, hi = -170, 170

    def bxp(t):
        return bx + bw * (t - tlo) / (thi - tlo)

    def byp(g):
        return by + bh * (1 - (g - lo) / (hi - lo))

    s.append(f'<text x="{bx}" y="{by-16}" font-size="13" font-weight="600" fill="{INK}">'
             'Dry window: weight vs filtered temperature, deviation from mean</text>')
    for g in range(-150, 151, 50):
        y = byp(g)
        s.append(f'<line x1="{bx}" y1="{y:.1f}" x2="{bx+bw}" y2="{y:.1f}" stroke="{GRID}"/>')
        s.append(f'<text x="{bx-8}" y="{y+4:.1f}" font-size="11" text-anchor="end" '
                 f'fill="{MUTED}">{g:+d} g</text>')
    for c in range(20, 35, 2):
        if tlo <= c <= thi:
            x = bxp(c)
            s.append(f'<line x1="{x:.1f}" y1="{by}" x2="{x:.1f}" y2="{by+bh}" stroke="{GRID}"/>')
            s.append(f'<text x="{x:.1f}" y="{by+bh+16}" font-size="11" text-anchor="middle" '
                     f'fill="{MUTED}">{c}&#176;C</text>')

    for t, g in zip(teff, dev_r):
        s.append(f'<circle cx="{bxp(t):.1f}" cy="{byp(g):.1f}" r="1.7" fill="{RAW}" opacity="0.5"/>')
    for t, g in zip(teff, dev_c):
        s.append(f'<circle cx="{bxp(t):.1f}" cy="{byp(g):.1f}" r="1.7" fill="{CORR}" opacity="0.5"/>')

    for dev, col, lab in ((dev_r, RAW, "uncompensated"), (dev_c, CORR, "compensated")):
        sl, icept, _ = ols(teff, dev)
        y1, y2 = byp(sl * tlo + icept), byp(sl * thi + icept)
        s.append(f'<line x1="{bx}" y1="{y1:.1f}" x2="{bx+bw}" y2="{y2:.1f}" stroke="{col}" '
                 f'stroke-width="2"/>')
        s.append(f'<text x="{bx+bw-6}" y="{min(max(y2, by+12), by+bh-6):.1f}" font-size="12" '
                 f'text-anchor="end" fill="{col}" font-weight="600">{lab}: {sl:+.2f} g/K</text>')

    s.append(f'<text x="{bx}" y="{by+bh+38}" font-size="11" fill="{MUTED}">'
             '&#963; 63.1 g &#8594; 11.6 g (5.5&#215;) &#183; raw series reconstructed from the '
             'uplinks by inverting the correction &#183; 746 samples</text>')
    s.append("</svg>")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(s))


if __name__ == "__main__":
    render(reconstruct(load(sys.argv[1])), sys.argv[2])
