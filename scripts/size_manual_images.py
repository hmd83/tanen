"""Ersetzt die Markdown-Bildsyntax im Benutzerhandbuch durch <img>-Tags mit
fester Breite. Ohne Breitenangabe rendern die hochkant-Fotos (Seitenverhaeltnis
0.46) ueber zwei Bildschirm- bzw. Seitenhoehen.

Einmalig ausgefuehrt; bleibt hier, falls neue Bilder dazukommen.
"""
import re
import pathlib

MD = pathlib.Path(__file__).resolve().parent.parent / "docs" / "BENUTZERHANDBUCH.md"

# Breite in px je Dateiname. Gewaehlt nach Seitenverhaeltnis, damit kein Bild
# hoeher als eine A4-Seite wird (Satzspiegel ca. 660 x 970 px bei 18 mm Rand).
WIDTHS = {
    # Hochkant, sehr schmal (0.37-0.46)
    "20260902_183139.jpg": 240,
    "test_with_block.jpeg": 240,
    "Screenshot_20260902_202330_Chrome.jpg": 250,
    "Screenshot_20260902_202342_Chrome.jpg": 250,
    "Screenshot_20260902_202411_Chrome.jpg": 250,
    "Screenshot_20260902_202428_Chrome.jpg": 250,
    "Screenshot_20260902_202454_Chrome.jpg": 250,
    "Screenshot_20260902_202522_Chrome.jpg": 250,
    "Screenshot_20260902_202543_Chrome.jpg": 220,
    # Hochkant, moderat (0.70)
    "Tanen base assambled.jpg": 300,
    # Querformat (2.16-2.42)
    "loadcell.jpeg": 520,
    "loadcell_2.jpeg": 520,
    "platform.jpeg": 520,
    "platform_2.jpeg": 520,
    "beep_platform.png": 600,
}

# [ \t]*$ statt \s*$ — \s frisst das Newline und damit die Leerzeile, die den
# folgenden Bildunterschrift-Blockquote abtrennt.
IMG_RE = re.compile(r"^([ \t]*)!\[([^\]]*)\]\(([^)]+)\)[ \t]*$", re.MULTILINE)


def replace(match: re.Match) -> str:
    indent, alt, src = match.group(1), match.group(2), match.group(3)
    name = src.rsplit("/", 1)[-1].replace("%20", " ")
    width = WIDTHS.get(name)
    if width is None:
        print(f"  WARN keine Breite fuer {name} - unveraendert")
        return match.group(0)
    print(f"  {name} -> width={width}")
    return f'{indent}<img src="{src}" alt="{alt}" width="{width}">'


def main() -> None:
    text = MD.read_text(encoding="utf-8")
    new_text, count = IMG_RE.subn(replace, text)
    if count:
        MD.write_text(new_text, encoding="utf-8")
    print(f"{count} Bilder umgestellt.")


if __name__ == "__main__":
    main()
