"""Outline the TanenBase wordmark (direction B) to SVG paths.

"Tanen" in M PLUS Rounded 1c ExtraBold, "Base" in Medium, letter-spacing
-0.01em, GPOS pair kerning applied, honey-pot mark placed left exactly as
drawn in site/logo.svg. Usage: outline_wordmark.py <font dir> <out dir>

Needs `pip install fonttools` and the two TTFs (OFL) from Google Fonts,
github.com/google/fonts/tree/main/ofl/mplusrounded1c:
MPLUSRounded1c-ExtraBold.ttf, MPLUSRounded1c-Medium.ttf.
Writes tanenbase-lockup.svg, tanenbase-lockup-dark.svg, tanenbase-wordmark.svg
(copy to site/; the wordmark to web/ too) and the black-only
tanenbase-lockup-black.svg, tanenbase-wordmark-black.svg (web/, for label.html).
"""
import sys
from pathlib import Path

from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

FONT_DIR, OUT_DIR = Path(sys.argv[1]), Path(sys.argv[2])
SIZE = 100.0            # px per em in the output
TRACK = -0.01           # em
MARK_RATIO = 1.3        # mark height / font size (as on the canvas: 120 / 92)
GAP_RATIO = 0.3         # gap mark -> text, x font size

MARK = (
    '<rect x="1.5" y="1.5" width="61" height="61" rx="16" fill="#ffffff" stroke="#000000" stroke-width="3"/>'
    '<g transform="translate(13.8, 5.6) scale(0.76)" fill="none" stroke="#000000" '
    'stroke-linecap="round" stroke-linejoin="round">'
    '<path d="M 12 18 C 12 12, 36 12, 36 18 C 36 22, 32 24, 32 26 C 38 28, 42 34, 42 42 '
    'C 42 52, 34 56, 24 56 C 14 56, 6 52, 6 42 C 6 34, 10 28, 16 26 C 16 24, 12 22, 12 18 Z" stroke-width="3.4"/>'
    '<ellipse cx="24" cy="18" rx="10" ry="3" stroke-width="2.8"/>'
    '<path d="M 14 32 Q 18 38 22 32 Q 26 42 30 32" stroke-width="2.8"/>'
    '</g>'
)


def pair_kerning(font):
    """{(left, right): xAdvance adjust} from GPOS 'kern' PairPos lookups."""
    kern = {}
    if "GPOS" not in font:
        return kern
    gpos = font["GPOS"].table
    idx = set()
    for fr in gpos.FeatureList.FeatureRecord:
        if fr.FeatureTag == "kern":
            idx.update(fr.Feature.LookupListIndex)
    for i in idx:
        lookup = gpos.LookupList.Lookup[i]
        for st in lookup.SubTable:
            if lookup.LookupType == 9:
                st = st.ExtSubTable
            if getattr(st, "LookupType", 2) != 2:
                continue
            cov = st.Coverage.glyphs
            if st.Format == 1:
                for g1, ps in zip(cov, st.PairSet):
                    for pvr in ps.PairValueRecord:
                        v = getattr(pvr.Value1, "XAdvance", 0) or 0
                        kern.setdefault((g1, pvr.SecondGlyph), v)
            elif st.Format == 2:
                c1 = st.ClassDef1.classDefs
                c2 = st.ClassDef2.classDefs
                used = {font.getBestCmap()[ord(c)] for c in "TanenBase"}
                for g1 in cov:
                    if g1 not in used:
                        continue
                    row = st.Class1Record[c1.get(g1, 0)]
                    for g2 in used:
                        rec = row.Class2Record[c2.get(g2, 0)]
                        v = getattr(rec.Value1, "XAdvance", 0) or 0
                        if v:
                            kern.setdefault((g1, g2), v)
    return kern


def run(font, text, x0, scale, kern):
    """Path data for `text` starting at x0 (px), baseline y=0, y down. Returns (d, end_x, bounds)."""
    cmap, gs, hmtx = font.getBestCmap(), font.getGlyphSet(), font["hmtx"]
    upm = font["head"].unitsPerEm
    x, parts, names = x0, [], [cmap[ord(c)] for c in text]
    bmin, bmax = float("inf"), float("-inf")
    for i, g in enumerate(names):
        pen = SVGPathPen(gs)
        gs[g].draw(TransformPen(pen, (scale, 0, 0, -scale, x, 0)))
        parts.append(pen.getCommands())
        bp = BoundsPen(gs)
        gs[g].draw(bp)
        if bp.bounds:
            bmin, bmax = min(bmin, bp.bounds[1]), max(bmax, bp.bounds[3])
        adv = hmtx[g][0]
        if i + 1 < len(names):
            adv += kern.get((g, names[i + 1]), 0)
        x += (adv + TRACK * upm) * scale
    return " ".join(parts), x - TRACK * upm * scale, (bmin * scale, bmax * scale)


def build(bold, medium, with_mark, tanen_fill, base_fill, ground=None):
    sb = SIZE / bold["head"].unitsPerEm
    sm = SIZE / medium["head"].unitsPerEm
    mark_px = SIZE * MARK_RATIO
    x0 = mark_px + SIZE * GAP_RATIO if with_mark else 0.0
    d1, x1, b1 = run(bold, "Tanen", x0, sb, pair_kerning(bold))
    # the bold/medium seam: no cross-font kerning, keep the tracking
    d2, x2, b2 = run(medium, "Base", x1 + TRACK * SIZE, sm, pair_kerning(medium))
    top, bottom = -max(b1[1], b2[1]), -min(b1[0], b2[0])   # y down
    text_h = bottom - top
    height = max(mark_px, text_h) if with_mark else text_h
    pad = SIZE * 0.1 if with_mark else 0.0   # the bare wordmark sits flush (UI headers)
    ty = pad + (height - text_h) / 2 - top            # baseline
    w, h = x2 + 2 * pad, height + 2 * pad
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w:.1f} {h:.1f}" '
           f'width="{w:.0f}" height="{h:.0f}" role="img" aria-label="TanenBase">',
           "<title>TanenBase</title>"]
    if ground:
        out.append(f'<rect width="100%" height="100%" fill="{ground}"/>')
    if with_mark:
        my = pad + (height - mark_px) / 2
        out.append(f'<g transform="translate({pad:.2f} {my:.2f}) scale({mark_px / 64:.5f})">{MARK}</g>')
    out.append(f'<g transform="translate({pad:.2f} {ty:.2f})">'
               f'<path fill="{tanen_fill}" d="{d1}"/><path fill="{base_fill}" d="{d2}"/></g>')
    out.append("</svg>")
    return "\n".join(out) + "\n"


bold = TTFont(FONT_DIR / "MPLUSRounded1c-ExtraBold.ttf")
medium = TTFont(FONT_DIR / "MPLUSRounded1c-Medium.ttf")
OUT_DIR.mkdir(parents=True, exist_ok=True)
files = {
    "tanenbase-lockup.svg": build(bold, medium, True, "#0c1017", "#39445a"),
    "tanenbase-lockup-dark.svg": build(bold, medium, True, "#ffffff", "#b4bdcf"),
    "tanenbase-wordmark.svg": build(bold, medium, False, "#0c1017", "#39445a"),
    # black only, for the thermal label printer (grey would dither)
    "tanenbase-lockup-black.svg": build(bold, medium, True, "#000000", "#000000"),
    "tanenbase-wordmark-black.svg": build(bold, medium, False, "#000000", "#000000"),
}
for name, svg in files.items():
    (OUT_DIR / name).write_text(svg, encoding="utf-8")
    print(name, len(svg.encode()), "bytes")
