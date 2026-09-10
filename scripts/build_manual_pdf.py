"""Baut aus docs/BENUTZERHANDBUCH.md eine Druckfassung (HTML, A4-tauglich).

    python scripts/build_manual_pdf.py           # nur HTML
    python scripts/build_manual_pdf.py --pdf     # zusaetzlich das PDF

Erzeugt docs/BENUTZERHANDBUCH.html. Das PDF ist seit 2026-09-10 nicht mehr im
Repo (es lief bei jeder Textaenderung als 2,4-MB-Binaerdatei mit) — wer eines
braucht, baut es mit --pdf oder druckt die HTML-Fassung aus dem Browser.
Braucht markdown-it-py, Pillow und fuer --pdf Chrome oder Edge.

Die HTML-Fassung verweist auf die Originalbilder in media/ und bleibt damit im
Repo gueltig — wer Seitenzahlen im Ausdruck will, oeffnet sie und druckt aus dem
Browser. Fuer das PDF werden die Fotos vorher auf Druckaufloesung verkleinert;
sonst bettet Chrome die 2-3 MB grossen Originale unveraendert ein.

Chrome laeuft headless mit --print-to-pdf; eigene Kopf-/Fusszeilen sind dabei
nicht moeglich, deshalb traegt jedes Kapitel seine Nummer in der Ueberschrift.
"""
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import unquote

from markdown_it import MarkdownIt
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
MD = DOCS / "BENUTZERHANDBUCH.md"
HTML = DOCS / "BENUTZERHANDBUCH.html"
PDF = DOCS / "BENUTZERHANDBUCH.pdf"

# Zielaufloesung = Anzeigebreite x SCALE. 2.5 entspricht ca. 240 dpi im Druck —
# fuer Fotos und Screenshots in einem Handbuch mehr als ausreichend.
SCALE = 2.5
JPEG_QUALITY = 85

BROWSERS = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
]

CSS = """
@page { size: A4; margin: 18mm 16mm 16mm 16mm; }

:root {
  --ink:      #1a202c;
  --muted:    #4a5568;
  --line:     #cbd5e0;
  --honey:    #d69e2e;
  --honey-bg: #fffbeb;
  --danger:   #9b2c2c;
}

* { box-sizing: border-box; }

body {
  font-family: "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
  font-size: 10.5pt;
  line-height: 1.5;
  color: var(--ink);
  max-width: 190mm;
  margin: 0 auto;
  padding: 12mm 8mm;
  hyphens: auto;
}

/* ---------- Ueberschriften ---------- */
h1 {
  font-size: 24pt;
  line-height: 1.2;
  margin: 0 0 4mm;
  padding-bottom: 3mm;
  border-bottom: 3px solid var(--honey);
}
h2 {
  font-size: 16pt;
  margin: 0 0 4mm;
  padding: 2mm 0 2mm 3mm;
  border-left: 5px solid var(--honey);
  background: var(--honey-bg);
  break-after: avoid;
  page-break-after: avoid;
}
h3 {
  font-size: 12.5pt;
  margin: 7mm 0 2mm;
  color: #2d3748;
  break-after: avoid;
  page-break-after: avoid;
}
h4 { font-size: 11pt; margin: 5mm 0 2mm; break-after: avoid; page-break-after: avoid; }

/* Jedes Kapitel auf eine neue Seite; das erste nicht. */
h2 { break-before: page; page-break-before: always; }
h2:first-of-type { break-before: auto; page-break-before: auto; }

p { margin: 0 0 3mm; orphans: 3; widows: 3; }

/* ---------- Listen ---------- */
ul, ol { margin: 0 0 3mm; padding-left: 6mm; }
li { margin-bottom: 1.5mm; }
li > ul, li > ol { margin-top: 1.5mm; }

/* ---------- Bilder ---------- */
img {
  display: block;
  max-width: 100%;
  height: auto;
  margin: 3mm 0 1mm;
  border: 1px solid var(--line);
  border-radius: 3px;
  break-inside: avoid;
  page-break-inside: avoid;
}

/* ---------- Bildunterschriften / Hinweise (Blockquotes) ---------- */
blockquote {
  margin: 0 0 4mm;
  padding: 2.5mm 4mm;
  border-left: 3px solid var(--line);
  background: #f7fafc;
  color: var(--muted);
  font-size: 9.5pt;
  break-inside: avoid;
  page-break-inside: avoid;
}
blockquote p { margin: 0 0 1.5mm; }
blockquote p:last-child { margin-bottom: 0; }

/* ---------- Tabellen ---------- */
table {
  width: 100%;
  border-collapse: collapse;
  margin: 0 0 4mm;
  font-size: 9.5pt;
  break-inside: auto;
}
th, td {
  border: 1px solid var(--line);
  padding: 1.8mm 2.5mm;
  text-align: left;
  vertical-align: top;
}
th { background: #edf2f7; font-weight: 600; }
tr { break-inside: avoid; page-break-inside: avoid; }
thead { display: table-header-group; }

/* ---------- Sonstiges ---------- */
hr { border: 0; border-top: 1px solid var(--line); margin: 6mm 0; }
code {
  font-family: "Cascadia Mono", Consolas, monospace;
  font-size: 9pt;
  background: #edf2f7;
  padding: 0.5mm 1.2mm;
  border-radius: 2px;
}
a { color: #1a202c; text-decoration: underline; }
strong { font-weight: 600; }

/* Inhaltsverzeichnis: im Druck sind Sprungmarken nutzlos, aber die Gliederung
   bleibt als Uebersicht wertvoll. */
h2#inhalt + ol { font-size: 11pt; }

@media print {
  body { padding: 0; max-width: none; }
  a { text-decoration: none; }
}
"""


def find_browser() -> str:
    for path in BROWSERS:
        if pathlib.Path(path).exists():
            return path
    for name in ("chrome", "msedge"):
        found = shutil.which(name)
        if found:
            return found
    sys.exit("Kein Chrome/Edge gefunden — PDF-Export nicht moeglich.")


def render_body() -> str:
    md = MarkdownIt("commonmark", {"html": True, "typographer": False})
    md.enable("table")
    return md.render(MD.read_text(encoding="utf-8"))


def wrap(body: str) -> str:
    return (
        "<!doctype html>\n"
        '<html lang="de">\n<head>\n<meta charset="utf-8">\n'
        "<title>TanenBase Stockwaage — Benutzerhandbuch</title>\n"
        f"<style>{CSS}</style>\n</head>\n<body>\n{body}\n</body>\n</html>\n"
    )


def build_html(body: str) -> None:
    HTML.write_text(wrap(body), encoding="utf-8")
    print(f"HTML: {HTML}  ({HTML.stat().st_size / 1024:.0f} KB, Originalbilder)")


def shrink_images(body: str, workdir: pathlib.Path) -> str:
    """Verkleinert jedes Bild auf Druckaufloesung und biegt die src-Pfade um."""
    assets = workdir / "img"
    assets.mkdir(parents=True, exist_ok=True)
    saved_before = saved_after = 0

    for tag in re.finditer(r'<img src="([^"]+)"[^>]*width="(\d+)"', body):
        src, width = tag.group(1), int(tag.group(2))
        source = (DOCS / unquote(src)).resolve()
        if not source.exists():
            print(f"  WARN fehlt: {source}")
            continue

        target_w = int(width * SCALE)
        with Image.open(source) as im:
            saved_before += source.stat().st_size
            if im.width > target_w:
                height = round(im.height * target_w / im.width)
                im = im.resize((target_w, height), Image.LANCZOS)
            out = assets / source.name.replace(" ", "_")
            if out.suffix.lower() in (".jpg", ".jpeg"):
                im.convert("RGB").save(out, quality=JPEG_QUALITY, optimize=True)
            else:
                im.save(out, optimize=True)
        saved_after += out.stat().st_size
        body = body.replace(f'src="{src}"', f'src="img/{out.name}"')

    print(
        f"Bilder: {saved_before / 1024 / 1024:.1f} MB -> "
        f"{saved_after / 1024 / 1024:.1f} MB"
    )
    return body


def build_pdf(body: str) -> None:
    browser = find_browser()
    with tempfile.TemporaryDirectory(prefix="tanen_manual_") as tmp:
        workdir = pathlib.Path(tmp)
        print_html = workdir / "print.html"
        print_html.write_text(wrap(shrink_images(body, workdir)), encoding="utf-8")

        if PDF.exists():
            PDF.unlink()
        cmd = [
            browser,
            "--headless=new",
            "--disable-gpu",
            "--no-sandbox",
            "--no-pdf-header-footer",
            "--run-all-compositor-stages-before-draw",
            "--virtual-time-budget=20000",
            f"--print-to-pdf={PDF}",
            print_html.as_uri(),
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=180)

    if not PDF.exists():
        print(result.stdout, result.stderr, sep="\n")
        sys.exit("PDF wurde nicht erzeugt.")
    print(f"PDF:  {PDF}  ({PDF.stat().st_size / 1024:.0f} KB)")


if __name__ == "__main__":
    rendered = render_body()
    build_html(rendered)
    if "--pdf" in sys.argv[1:]:
        build_pdf(rendered)
    else:
        print("PDF:  uebersprungen (mit --pdf bauen)")
