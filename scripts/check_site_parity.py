"""Prueft, ob die drei Sprachfassungen von tanen.eu gleich gebaut sind.

    python scripts/check_site_parity.py            # site/ im Repo
    python scripts/check_site_parity.py <ordner>   # Ordner mit index.html, en/, ar/

Die Startseite gibt es dreimal: / (Deutsch), /en/ und /ar/, jede als eigene,
fertige Seite. Damit sie nicht wieder auseinanderlaufen, wird verglichen:
  - derselbe <style>-Block, Byte fuer Byte
  - dieselben Kopf-Tags (meta/link nach name, property, rel, hreflang)
  - dasselbe Geruest: Elemente mit id, class, role, dir, lang, data-i18n*
    und internen Links (../ abgezogen); nur Flagge und Name der eigenen
    Sprache im Sprachknopf duerfen abweichen
  - dasselbe Skript bis auf das Woerterbuch und PAGE_LANG
  - ein Woerterbuch je Seite, in ihrer Sprache, mit denselben Schluesseln
  - jeder data-i18n-Text und jedes data-i18n-al/-alt/-title steht im HTML
    genau so wie im Woerterbuch, ebenso <title> und meta description -
    sonst springt beim Laden Text um, oder ein fehlender Schluessel wird
    roh angezeigt
  - sichtbarer Text ausserhalb von data-i18n nur aus ALLOWED (Marken,
    Namen, Zahlen); alles andere gehoert ins Woerterbuch

Je Pruefung eine Zeile PASS/FAIL, Exit-Code 1 bei mindestens einem FAIL.
Nur Standardbibliothek.
"""
import pathlib
import re
import sys
from html.parser import HTMLParser

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

ROOT = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parent.parent / 'site'
PAGES = {'de': 'index.html', 'en': 'en/index.html', 'ar': 'ar/index.html'}
DIRS = {'de': 'ltr', 'en': 'ltr', 'ar': 'rtl'}
KEEP = ('id', 'class', 'role', 'dir', 'lang', 'data-i18n', 'data-i18n-al', 'data-i18n-alt', 'data-i18n-title')
I18N_ATTR = {'data-i18n-al': 'aria-label', 'data-i18n-alt': 'alt', 'data-i18n-title': 'title'}
ALLOWED = {
    'Language', 'Deutsch', 'English', 'العربية', 'TanenBase', '© 2026 TanenBase', 'Hussein Daj',
    'info@tanen.eu', 'BEEP', 'beelogger', 'b.tree', 'Seeed Studio', 'LinkedIn', 're@di — regional.digital',
    'SBS-PF-150', 'xiSerge', 'Pixabay', 'OpenStreetMap', 'Tanen_Base_pcb/', 'µA', 'g', '€',
}
NUMERIC = re.compile(r'^[\d\s~.,±:/·©€%+\-–—]*$')
KV = re.compile(r"(?<![\w$])([A-Za-z_][A-Za-z0-9_]*)\s*:\s*'((?:[^'\\\n]|\\.)*)'")
VOID = {'area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'source', 'track', 'wbr'}

failed = 0


def check(ok, what):
    global failed
    print(('PASS ' if ok else 'FAIL ') + what)
    if not ok:
        failed += 1
    return ok


def norm(s):
    return ' '.join(s.split())


def unjs(v):
    return re.sub(r'\\(.)', r'\1', v)


class Page(HTMLParser):
    def __init__(self, code):
        super().__init__(convert_charrefs=True)
        self.code = code
        self.html = {}
        self.head_tags = []
        self.title = ''
        self.desc = None
        self.skeleton = []
        self.texts = []          # (key, statischer Text)
        self.attrs = []          # (key, Attribut, Wert)
        self.loose = []
        self._stack = []         # [tag, key, buf]
        self._depth = 0
        self._head = self._body = self._in_title = False
        self._skip = 0
        self._langbtn = False

    def handle_starttag(self, tag, attrs):
        a = {k: (v or '') for k, v in attrs}
        if tag == 'html':
            self.html = a
            return
        if tag == 'head':
            self._head = True
            return
        if tag == 'body':
            self._head, self._body = False, True
            return
        if self._head:
            if tag in ('meta', 'link'):
                self.head_tags.append((tag,) + tuple(f'{k}={a[k]}' for k in
                                                     ('charset', 'name', 'property', 'rel', 'hreflang') if k in a))
                if a.get('name') == 'description':
                    self.desc = a.get('content', '')
            elif tag == 'title':
                self._in_title = True
            return
        if not self._body:
            return
        if tag in ('script', 'style'):
            self._skip += 1
            return
        if self._skip:
            return
        bits = [tag] + [f'{k}={a[k]}' for k in KEEP if k in a]
        for k in ('href', 'src'):
            v = a.get(k, '')
            if v and not v.startswith(('http:', 'https:', 'mailto:')):
                bits.append(f'{k}={v[3:] if v.startswith("../") else v}')
        line = '  ' * self._depth + ' '.join(bits)
        if self._langbtn:
            # Der Sprachknopf zeigt Flagge und Namen der eigenen Sprache.
            classes = a.get('class', '').split()
            if 'flag' in classes:
                line = line.replace(f'flag-{self.code}', 'flag-*')
            if 'lang-name' in classes:
                line = line.replace(f'lang={self.code}', 'lang=*')
        self.skeleton.append(line)
        if tag == 'button' and 'lang-btn' in a.get('class', '').split():
            self._langbtn = True
        for k, at in I18N_ATTR.items():
            if k in a:
                self.attrs.append((a[k], at, a.get(at)))
        if tag in VOID:
            return
        self._depth += 1
        self._stack.append([tag, a.get('data-i18n'), []])

    def handle_endtag(self, tag):
        if self._head and tag == 'title':
            self._in_title = False
        if not self._body:
            return
        if tag in ('script', 'style'):
            self._skip -= 1
            return
        if self._skip or tag in VOID:
            return
        if tag == 'button':
            self._langbtn = False
        while self._stack:
            t, key, buf = self._stack.pop()
            self._depth -= 1
            if key:
                self.texts.append((key, norm(''.join(buf))))
            if self._stack:
                self._stack[-1][2].extend(buf)
            if t == tag:
                break

    def handle_data(self, data):
        if self._in_title:
            self.title += data
            return
        if not self._body or self._skip:
            return
        if self._stack:
            self._stack[-1][2].append(data)
        if data.strip() and not any(key for _, key, _ in self._stack):
            self.loose.append(norm(data))


def scripts_of(src, code):
    """Inline-Skripte ohne JSON-LD; Woerterbuch und PAGE_LANG ausgeblendet."""
    out = []
    for m in re.finditer(r'<script(\s[^>]*)?>(.*?)</script>', src, re.S):
        if 'application/ld+json' in (m.group(1) or ''):
            continue
        body = re.sub(r'const I18N = \{\n.*?\n\};\n', 'const I18N = {...};\n', m.group(2), flags=re.S)
        body = body.replace(f"const PAGE_LANG = '{code}';", "const PAGE_LANG = '*';")
        out.append(body)
    return out


def dicts_of(src):
    m = re.search(r'const I18N = \{\n(.*?)\n\};\n', src, re.S)
    if not m:
        return {}
    out = {}
    for b in re.finditer(r'\n(\w+): \{\n(.*?)\n\}', '\n' + m.group(1), re.S):
        out[b.group(1)] = {k: unjs(v) for k, v in KV.findall(b.group(2))}
    return out


src, pages, dicts = {}, {}, {}
for code, rel in PAGES.items():
    path = ROOT / rel
    if not check(path.is_file(), f'{rel} vorhanden'):
        continue
    src[code] = path.read_text(encoding='utf-8')
    pages[code] = Page(code)
    pages[code].feed(src[code])
    dicts[code] = dicts_of(src[code])
if len(src) != len(PAGES):
    sys.exit(1)

ref = 'de'
css = {c: re.search(r'<style>(.*?)</style>', s, re.S).group(1) for c, s in src.items()}
for c in ('en', 'ar'):
    check(css[c] == css[ref], f'{PAGES[c]}: <style> gleich wie {PAGES[ref]}')
    check(pages[c].head_tags == pages[ref].head_tags, f'{PAGES[c]}: Kopf-Tags gleich wie {PAGES[ref]}')
    same = pages[c].skeleton == pages[ref].skeleton
    if not same:
        a, b = pages[ref].skeleton, pages[c].skeleton
        i = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
        print(f'     erste Abweichung, Element {i}:\n     {PAGES[ref]}: {a[i].strip() if i < len(a) else "-"}'
              f'\n     {PAGES[c]}: {b[i].strip() if i < len(b) else "-"}')
    check(same, f'{PAGES[c]}: Geruest gleich wie {PAGES[ref]}')
    check(scripts_of(src[c], c) == scripts_of(src[ref], ref),
          f'{PAGES[c]}: Skript gleich wie {PAGES[ref]} (ohne Woerterbuch und PAGE_LANG)')

for c, p in pages.items():
    rel = PAGES[c]
    check(p.html.get('lang') == c and p.html.get('dir') == DIRS[c],
          f'{rel}: <html lang="{p.html.get("lang")}" dir="{p.html.get("dir")}">')
    d = dicts[c]
    if not check(list(d) == [c], f'{rel}: genau ein Woerterbuch, "{c}" ({", ".join(d) or "keins"})'):
        continue
    words = d[c]
    check(words.get('_dir') == DIRS[c], f'{rel}: _dir {words.get("_dir")}')
    if c != ref and ref in dicts[ref]:
        extra = sorted(set(words) - set(dicts[ref][ref]))
        miss = sorted(set(dicts[ref][ref]) - set(words))
        check(not extra and not miss, f'{rel}: Schluessel wie {PAGES[ref]} {("fehlt " + str(miss)) if miss else ""}'
                                      f'{(" zusaetzlich " + str(extra)) if extra else ""}'.rstrip())
    bad = [(k, s) for k, s in p.texts if norm(words.get(k, '\0')) != s]
    for k, s in bad[:10]:
        print(f'     [{k}] HTML: {s[:90]!r}\n     {" " * len(k)}   Woerterbuch: {words.get(k, "<fehlt>")[:90]!r}')
    check(not bad, f'{rel}: data-i18n-Texte = Woerterbuch ({len(p.texts)} geprueft)')
    bad = [(k, at, v) for k, at, v in p.attrs if words.get(k) != v]
    for k, at, v in bad[:10]:
        print(f'     [{k}] {at}: {v!r} / Woerterbuch: {words.get(k, "<fehlt>")!r}')
    check(not bad, f'{rel}: data-i18n-Attribute = Woerterbuch ({len(p.attrs)} geprueft)')
    check(norm(p.title) == norm(words.get('pageTitle', '')), f'{rel}: <title> = pageTitle')
    check(norm(p.desc or '') == norm(words.get('metaDesc', '')), f'{rel}: meta description = metaDesc')
    stray = sorted({t for t in p.loose if t not in ALLOWED and not NUMERIC.match(t)})
    check(not stray, f'{rel}: kein sichtbarer Text ausserhalb des Woerterbuchs {stray or ""}'.rstrip())

print(f'\n{failed} FAIL' if failed else '\nalles PASS')
sys.exit(1 if failed else 0)
