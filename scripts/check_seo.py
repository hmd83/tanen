"""Prueft, ob tanen.eu fuer Suchmaschinen noch so dasteht wie gewollt.

    python scripts/check_seo.py                         # live: https://tanen.eu
    python scripts/check_seo.py http://localhost:8000   # lokal: python -m http.server -d site 8000

Je Pruefung eine Zeile PASS/FAIL, Exit-Code 1 bei mindestens einem FAIL.
Nur Standardbibliothek.

Geprueft: HTTP 200 fuer /, /en/, /ar/, /impressum.html, /datenschutz.html,
/sitemap.xml, /robots.txt; lang-Attribut, canonical, og:url und og:locale je
Sprachseite; alle vier hreflang-Links vorhanden, keiner auf eine ?lang=-Adresse,
jedes Ziel liefert 200; Titel- und Beschreibungslaenge; die Suchbegriffe im
sichtbaren Text je Sprache (nicht nur in meta keywords); die Sprachwahl als
echte Links; JSON-LD gueltig und mit passender inLanguage; jede Sitemap-Adresse
erreichbar, selbst-kanonisch und ohne noindex.

Canonical und hreflang tragen immer https://tanen.eu - bei einer lokalen
Basis werden die Ziele auf diese umgebogen. Vor tanen.eu sitzt ein
Proxy-Cache: direkt nach dem Hochladen kann eine Seite noch kurz alt sein.
"""
import json
import re
import sys
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from html.parser import HTMLParser

PROD = 'https://tanen.eu'
PAGES = {'/': 'de', '/en/': 'en', '/ar/': 'ar'}
LEGAL = ['/impressum.html', '/datenschutz.html']
HREFLANG = {'de': '/', 'en': '/en/', 'ar': '/ar/', 'x-default': '/'}
LOCALES = {'de': 'de_DE', 'en': 'en_GB', 'ar': 'ar_AR'}
KEYWORDS = {
    'de': ['stockwaage', 'bienenwaage', 'lorawan'],
    'en': ['open source hive scale', 'lorawan hive scale', 'beehive monitoring'],
    'ar': ['ميزان خلايا', 'lorawan', 'apache'],
}
MAX_TITLE = 65
MAX_DESC = 170

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
    """lang, canonical, hreflang, Metas, JSON-LD, Links und sichtbarer Text."""

    def __init__(self):
        super().__init__()
        self.lang = None
        self.canonical = None
        self.hreflang = {}
        self.meta = {}
        self.title = None
        self._in_title = False
        self.links = []
        self.jsonld = []
        self._in_ld = False
        self.text = []
        self._body = False
        self._skip = 0

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == 'html':
            self.lang = a.get('lang')
        elif tag == 'link':
            rel = (a.get('rel') or '').lower()
            if rel == 'canonical':
                self.canonical = a.get('href')
            elif rel == 'alternate' and 'hreflang' in a:
                self.hreflang[a['hreflang']] = a.get('href', '')
        elif tag == 'meta':
            key = a.get('property') or a.get('name')
            if key:
                self.meta[key.lower()] = a.get('content', '')
        elif tag == 'a':
            self.links.append(a.get('href', ''))
        elif tag == 'title':
            self._in_title = True
            self.title = ''
        elif tag == 'script' and a.get('type') == 'application/ld+json':
            self._in_ld = True
        elif tag in ('script', 'style'):
            self._skip += 1
        elif tag == 'body':
            self._body = True

    def handle_endtag(self, tag):
        if tag == 'title':
            self._in_title = False
        elif tag in ('script', 'style'):
            if self._in_ld:
                self._in_ld = False
            elif self._skip:
                self._skip -= 1

    def handle_data(self, data):
        if self._in_title:
            self.title = (self.title or '') + data
        elif self._in_ld:
            self.jsonld.append(data)
        elif self._body and not self._skip:
            self.text.append(data)


status = {}
for path in ['/sitemap.xml', '/robots.txt', *PAGES, *LEGAL]:
    status[path] = fetch(path)
    check(status[path][0] == 200, f'{path}: HTTP {status[path][0]}')

pages = {}
for path, lang in PAGES.items():
    if status[path][0] != 200:
        continue
    p = Page()
    p.feed(status[path][1])
    pages[path] = p
    check(p.lang == lang, f'{path}: <html lang="{p.lang}">, erwartet "{lang}"')
    check(p.canonical == PROD + path, f'{path}: canonical {p.canonical}')
    check(p.meta.get('og:url') == PROD + path, f'{path}: og:url {p.meta.get("og:url")}')
    check(p.meta.get('og:locale') == LOCALES[lang], f'{path}: og:locale {p.meta.get("og:locale")}')
    check(bool(p.meta.get('og:title')), f'{path}: og:title vorhanden')
    title = ' '.join((p.title or '').split())
    desc = ' '.join(p.meta.get('description', '').split())
    check(25 <= len(title) <= MAX_TITLE, f'{path}: Titel {len(title)} Zeichen')
    check(80 <= len(desc) <= MAX_DESC, f'{path}: Beschreibung {len(desc)} Zeichen')
    for hl, target in HREFLANG.items():
        href = p.hreflang.get(hl)
        check(href == PROD + target, f'{path}: hreflang {hl} -> {href}')
    bad = [h for h in p.hreflang.values() if '?lang=' in h]
    check(not bad, f'{path}: kein hreflang auf ?lang= {bad or ""}'.rstrip())
    for target in dict.fromkeys(HREFLANG.values()):
        check(target in p.links, f'{path}: Sprachwahl verlinkt {target}')
    text = ' '.join(' '.join(p.text).split()).lower()
    for k in KEYWORDS[lang]:
        check(k.lower() in text, f'{path}: Text enthaelt "{k}"')
    if lang == 'ar':
        check(re.search(r'[\u0600-\u06FF]', text) is not None, '/ar/: sichtbarer Text ist arabisch')
        check('ihr bienenvolk gibt ihnen entwarnung' not in text, '/ar/: kein deutscher Ueberschriftstext')

    if p.jsonld:
        try:
            data = json.loads(''.join(p.jsonld))
            nodes = data.get('@graph', [data])
            check(True, f'{path}: JSON-LD gueltig')
            web = [n for n in nodes if n.get('@type') == 'WebPage']
            check(bool(web) and web[0].get('inLanguage') == lang,
                  f'{path}: JSON-LD WebPage inLanguage={web[0].get("inLanguage") if web else None}')
            for n in nodes:
                if n.get('@type') == 'BlogPosting':
                    check(bool(n.get('author') and n.get('datePublished')),
                          f'{path}: BlogPosting nur mit author und datePublished')
        except json.JSONDecodeError as e:
            check(False, f'{path}: JSON-LD kaputt ({e})')
    else:
        check(False, f'{path}: kein JSON-LD')

targets = sorted(set(HREFLANG.values()))
for t in dict.fromkeys(targets):
    code = status[t][0] if t in status else fetch(t)[0]
    check(code == 200, f'hreflang-Ziel {base + t}: HTTP {code}')

if status['/sitemap.xml'][0] == 200:
    try:
        root = ET.fromstring(status['/sitemap.xml'][1])
        ns = {'s': 'http://www.sitemaps.org/schemas/sitemap/0.9'}
        locs = [e.text.strip() for e in root.findall('s:url/s:loc', ns)]
        check(True, '/sitemap.xml: gueltiges XML')
        for path in PAGES:
            check(PROD + path in locs, f'/sitemap.xml: enthaelt {PROD + path}')
        check(PROD + '/phone-demo.html' not in locs, '/sitemap.xml: keine noindex-Seite (phone-demo)')
        for loc in locs:
            path = loc[len(PROD):] if loc.startswith(PROD) else loc
            if path not in status or status[path][0] != 200:
                status[path] = fetch(path)
            code, html_text = status[path]
            check(code == 200, f'/sitemap.xml: {path} erreichbar (HTTP {code})')
            if code != 200:
                continue
            q = Page()
            q.feed(html_text)
            check('noindex' not in q.meta.get('robots', '').lower(), f'/sitemap.xml: {path} ohne noindex')
            check(q.canonical == loc, f'/sitemap.xml: {path} canonical {q.canonical}')
    except ET.ParseError as e:
        check(False, f'/sitemap.xml: kein gueltiges XML ({e})')

if status['/robots.txt'][0] == 200:
    lines = [l.strip().lower() for l in status['/robots.txt'][1].splitlines()]
    check('disallow: /' not in lines, '/robots.txt: kein "Disallow: /"')
    check(f'sitemap: {PROD}/sitemap.xml' in lines, '/robots.txt: nennt die Sitemap')

print(f'\n{failed} FAIL' if failed else '\nalles PASS')
sys.exit(1 if failed else 0)
