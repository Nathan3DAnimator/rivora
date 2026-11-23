#!/usr/bin/env python3
"""The Assistant's knowledge base (notas-dev/assistente.md): assistant/articles/*.md and assistant/synonyms.txt into one
index.json (what the editor downloads: online on the first question, or kept for offline use) and a manifest.

    python tools/assistant_pack.py [out_dir]       (default: build/assistant)

An article (UTF-8):
    id: package-game
    kind: howto | recipe | error | concept
    title: Empacotar o jogo | Package the game
    keys: empacotar, pacote, exe, package, ship
    match: isn't declared here                      (kind error: a part of the compiler's message it explains)
    --- pt
    the text in Portuguese (Markdown: ## headings, 1. steps, - points, `code`, ```real ... ```, [label](action))
    --- en
    the text in English
Actions in links: show:Build>Package Game...  open:package|prefs|project|outliner|details|content|log|debug|api|ide|assistant
api:PlaySound2D  add:<actor>  run:play|build-lighting  article:<id>  url:https://...
"""
import json
import os
import re
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'assistant')


def read_article(path):
    text = open(path, encoding='utf-8').read().replace('\r\n', '\n')
    head, _, rest = text.partition('\n--- ')
    meta = {}
    for line in head.split('\n'):
        if ':' in line and not line.startswith('#'):
            k, v = line.split(':', 1)
            meta[k.strip()] = v.strip()
    bodies = {}
    for part in ('--- ' + rest).split('\n--- ') if rest else []:
        part = part[4:] if part.startswith('--- ') else part
        lang, _, body = part.partition('\n')
        bodies[lang.strip()] = body.strip() + '\n'
    if 'id' not in meta or 'title' not in meta:
        raise SystemExit('%s: needs id and title' % path)
    titles = [t.strip() for t in meta['title'].split('|')]
    a = {
        'id': meta['id'],
        'kind': meta.get('kind', 'howto'),
        'title': {'pt': titles[0], 'en': titles[1] if len(titles) > 1 else titles[0]},
        'keys': [k.strip() for k in meta.get('keys', '').split(',') if k.strip()],
        'body': {'pt': bodies.get('pt', ''), 'en': bodies.get('en', bodies.get('pt', ''))},
    }
    if meta.get('match'):
        a['match'] = [m.strip() for m in meta['match'].split('|') if m.strip()]
    for lang in ('pt', 'en'):
        for link in re.findall(r'\]\(([^)]+)\)', a['body'][lang]):
            if not re.match(r'^(show|open|api|add|run|article|url):', link):
                raise SystemExit('%s: unknown action "%s"' % (path, link))
        # the pictures the editor draws (assistant_draw.c): an unknown kind would show as plain text
        for kind in re.findall(r'^```diagram (\w+)', a['body'][lang], re.M):
            if kind not in DIAGRAMS:
                raise SystemExit('%s: unknown diagram "%s"' % (path, kind))
    return a


DIAGRAMS = {'blueprint', 'steps', 'keys', 'axes', 'layout', 'compare', 'bars'}


# every area of the engine (notas-dev/<area>.md) is explained by some article (assistant/coverage.txt): a new feature
# goes into the Assistant with it
def check_coverage(ids):
    cov = {}
    for line in open(os.path.join(SRC, 'coverage.txt'), encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if ':' in line:
            area, arts = line.split(':', 1)
            cov[area.strip()] = [x.strip() for x in arts.split(',') if x.strip()]
    notes = os.path.join(ROOT, 'notas-dev')
    for name in sorted(os.listdir(notes)) if os.path.isdir(notes) else []:
        area = name[:-3]
        if name.endswith('.md') and not cov.get(area):
            raise SystemExit('notas-dev/%s: no article explains it in the Assistant (assistant/coverage.txt)' % name)
    for area, arts in cov.items():
        for a in arts:
            if a not in ids:
                raise SystemExit('assistant/coverage.txt: %s names a missing article "%s"' % (area, a))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'assistant')
    os.makedirs(out, exist_ok=True)
    arts = []
    adir = os.path.join(SRC, 'articles')
    for name in sorted(os.listdir(adir)):
        if name.endswith('.md'):
            arts.append(read_article(os.path.join(adir, name)))
    ids = [a['id'] for a in arts]
    if len(ids) != len(set(ids)):
        raise SystemExit('two articles with the same id')
    for a in arts:  # the article: links point to existing ones
        for lang in ('pt', 'en'):
            for ref in re.findall(r'\]\(article:([^)]+)\)', a['body'][lang]):
                if ref not in ids:
                    raise SystemExit('%s: links to a missing article "%s"' % (a['id'], ref))
    check_coverage(set(ids))
    syn = []
    for line in open(os.path.join(SRC, 'synonyms.txt'), encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if line:
            syn.append([w.strip() for w in line.split(',') if w.strip()])
    data = json.dumps({'format': 'RealEngine Assistant', 'version': 1, 'articles': arts, 'synonyms': syn},
                      ensure_ascii=False, separators=(',', ':')).encode('utf-8')
    crc = zlib.crc32(data) & 0xFFFFFFFF
    stamp = '%08x' % crc
    open(os.path.join(out, 'index.json'), 'wb').write(data)
    open(os.path.join(out, 'manifest.ini'), 'w', encoding='utf-8').write(
        'version = %s\nbytes = %d\narticles = %d\n' % (stamp, len(data), len(arts)))
    print('assistant: %d articles, %d synonym groups, %d KB, version %s -> %s' % (len(arts), len(syn), len(data) // 1024, stamp, out))


if __name__ == '__main__':
    main()
