"""Prueft, ob tanen.eu fuer Suchmaschinen noch so dasteht wie gewollt.

    python scripts/check_seo.py                         # live: https://tanen.eu
    python scripts/check_seo.py http://localhost:8000   # lokal: python -m http.server -d site 8000

Je Pruefung eine Zeile PASS/FAIL, Exit-Code 1 bei mindestens einem FAIL.
Nur Standardbibliothek.

Geprueft: HTTP 200 fuer /, /en/, /ar/, /sitemap.xml, /robots.txt; lang-Attribut
und canonical je Sprachseite; alle vier hreflang-Links vorhanden, keiner auf
eine ?lang=-Adresse, jedes Ziel liefert 200; die Suchbegriffe im sichtbaren
Text von /en/ (nicht nur in meta keywords).

Canonical und hreflang tragen immer https://tanen.eu - bei einer lokalen
Basis werden die Ziele auf diese umgebogen. Vor tanen.eu sitzt ein
Proxy-Cache: direkt nach dem Hochladen kann eine Seite noch kurz alt sein.
"""
import sys
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from html.parser import HTMLParser

PROD = 'https://tanen.eu'
PAGES = {'/': 'de', '/en/': 'en', '/ar/': 'ar'}
HREFLANG = {'de': '/', 'en': '/en/', 'ar': '/ar/', 'x-default': '/'}
KEYWORDS = ['open source hive scale', 'LoRaWAN hive scale', 'beehive monitoring', 'TTN', 'Apache-2.0']

base = (sys.argv[1] if len(sys.argv) > 1 else PROD).rstrip('/')
failed = 0


def check(ok, what):
    global failed
    print(('PASS ' if ok else 'FAIL ') + what)
    if not ok:
        failed += 1
    return ok


def fetch(path):
    req = urllib.request.Request(base + path, headers={'User-Agent': 'tanen-check-seo'})
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, r.read().decode('utf-8', 'replace')
    except urllib.error.HTTPError as e:
        return e.code, ''
    except (urllib.error.URLError, TimeoutError) as e:
        return str(e), ''


class Page(HTMLParser):
    """lang, canonical, hreflang-Links und der sichtbare Text des body."""

    def __init__(self):
        super().__init__()
        self.lang = None
        self.canonical = None
        self.hreflang = {}
        self.text = []
        self._body = False
        self._skip = 0

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == 'html':
            self.lang = a.get('lang')
        elif tag == 'link' and a.get('rel') == 'canonical':
            self.canonical = a.get('href')
        elif tag == 'link' and a.get('rel') == 'alternate' and 'hreflang' in a:
            self.hreflang[a['hreflang']] = a.get('href', '')
        elif tag == 'body':
            self._body = True
        elif tag in ('script', 'style'):
            self._skip += 1

    def handle_endtag(self, tag):
        if tag in ('script', 'style') and self._skip:
            self._skip -= 1

    def handle_data(self, data):
        if self._body and not self._skip:
            self.text.append(data)


status = {}
for path in ['/sitemap.xml', '/robots.txt', *PAGES]:
    status[path] = fetch(path)
    check(status[path][0] == 200, f'{path}: HTTP {status[path][0]}')

for path, lang in PAGES.items():
    if status[path][0] != 200:
        continue
    p = Page()
    p.feed(status[path][1])
    check(p.lang == lang, f'{path}: <html lang="{p.lang}">, erwartet "{lang}"')
    check(p.canonical == PROD + path, f'{path}: canonical {p.canonical}')
    for hl, target in HREFLANG.items():
        href = p.hreflang.get(hl)
        check(href == PROD + target, f'{path}: hreflang {hl} -> {href}')
    bad = [h for h in p.hreflang.values() if '?lang=' in h]
    check(not bad, f'{path}: kein hreflang auf ?lang= {bad or ""}'.rstrip())
    if path == '/en/':
        text = ' '.join(' '.join(p.text).split()).lower()
        for k in KEYWORDS:
            check(k.lower() in text, f'/en/: Text enthaelt "{k}"')

targets = sorted(set(HREFLANG.values()))
for t in targets:
    code = status[t][0] if t in status else fetch(t)[0]
    check(code == 200, f'hreflang-Ziel {base + t}: HTTP {code}')

if status['/sitemap.xml'][0] == 200:
    try:
        root = ET.fromstring(status['/sitemap.xml'][1])
        ns = {'s': 'http://www.sitemaps.org/schemas/sitemap/0.9'}
        locs = {e.text.strip() for e in root.findall('s:url/s:loc', ns)}
        check(True, '/sitemap.xml: gueltiges XML')
        for path in PAGES:
            check(PROD + path in locs, f'/sitemap.xml: enthaelt {PROD + path}')
    except ET.ParseError as e:
        check(False, f'/sitemap.xml: kein gueltiges XML ({e})')

if status['/robots.txt'][0] == 200:
    lines = [l.strip().lower() for l in status['/robots.txt'][1].splitlines()]
    check('disallow: /' not in lines, '/robots.txt: kein "Disallow: /"')
    check(f'sitemap: {PROD}/sitemap.xml' in lines, '/robots.txt: nennt die Sitemap')

print(f'\n{failed} FAIL' if failed else '\nalles PASS')
sys.exit(1 if failed else 0)
