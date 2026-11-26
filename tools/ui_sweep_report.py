# The UI sweep's report (item 21; src/ui_sweep.c, notas-dev/testes.md):
#   python tools/ui_sweep_report.py <dir with ui_sweep.jsonl> [--src src]
# writes <dir>/ui_sweep.md (every control, OK or its problems) and <dir>/ui_sweep_bugs.md (one line per bug: the control,
# the error, the probable file). A control's probable file: the assert's file, or the source that draws its label (the
# label as shown is looked up in the translation tables back to its English msgid, then found in src/), or the msgid
# without translation found in src/.
import io, json, os, re, sys

def load_tables(src):
    back = {}  # the text as shown (pt or es) -> the English msgid
    pat_pt = re.compile(r'^\s*\{\s*"((?:[^"\\]|\\.)*)",\s*\{\s*"((?:[^"\\]|\\.)*)"')
    pat_es = re.compile(r'^\s*\{\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"')
    for name, pat in (('i18n_table.c', pat_pt), ('i18n_table_es.c', pat_es)):
        p = os.path.join(src, name)
        if not os.path.exists(p):
            continue
        for line in io.open(p, encoding='utf-8', errors='replace'):
            m = pat.match(line)
            if m:
                back.setdefault(m.group(2), m.group(1))
    return back

def index_sources(src):
    files = {}
    for root, _, names in os.walk(src):
        for n in names:
            if n.endswith(('.c', '.inc', '.h')) and not n.startswith(('i18n_table', 'ui_tips_table', 'ui_sweep')):
                p = os.path.join(root, n)
                try:
                    files[os.path.relpath(p, os.path.dirname(src)).replace('\\', '/')] = io.open(p, encoding='utf-8', errors='replace').read()
                except OSError:
                    pass
    return files

def find_in_sources(files, text):
    if not text or len(text) < 3:
        return None
    needle = '"' + text.replace('\\', '\\\\').replace('"', '\\"')
    for path, body in files.items():
        i = body.find(needle)
        if i >= 0:
            return '%s:%d' % (path, body.count('\n', 0, i) + 1)
    return None

def probable_file(rec, back, files):
    probs = rec.get('problems', '')
    m = re.search(r'IM_ASSERT\(.*\) ([\w.]+):(\d+)', probs)
    if m:
        return '%s:%s (o assert)' % (m.group(1), m.group(2))
    m = re.search(r'no translation: "([^"]+)"', probs)
    if m:
        f = find_in_sources(files, m.group(1))
        if f:
            return f + ' (o texto sem tradução)'
    parts = [p.strip() for p in rec['control'].split(' > ')]
    for label in reversed(parts):
        if label.startswith('[right click'):
            continue
        msgid = back.get(label, label)
        f = find_in_sources(files, msgid)
        if f:
            return f
    return '?'

# A "no translation" that isn't one: an id ("tb_save", "phys_sim_mode"), or a text already translated looked up again
# (TR of a TR, a tooltip's lookup of the label as shown: TR gives the text back, nothing wrong on screen). Unless that
# translated text is written in the code itself: a Portuguese literal passed to TR shows in Portuguese in English and
# Spanish (kept: "literal em pt passado ao TR"). --all-misses keeps every one.
ALL_MISSES = '--all-misses' in sys.argv

def literal_in_code(files, text):
    needle = 'TR("' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'  # (passed to TR as written)
    for path, body in files.items():
        i = body.find(needle)
        if i >= 0:
            return '%s:%d' % (path, body.count('\n', 0, i) + 1)
    return None

def clean(rec, back, files):
    parts = []
    for p in [x.strip() for x in rec.get('problems', '').split('; ') if x.strip()]:
        m = re.fullmatch(r'no translation: "(.*)"', p)
        if m and not ALL_MISSES:
            text = m.group(1)
            if re.fullmatch(r'[A-Za-z0-9_]+', text) and ('_' in text or text.islower()):
                continue
            # the data of the tests' assets (BP_TestActor's variables, BPI_Test, AT_Blip's components: names, not the UI)
            # and a new parameter's default name (Param1 / Result1: an identifier the scripts use, never translated)
            if re.search(r'(^|[ >/])[A-Z]+_?\w*Test\w*\.r\w+', rec.get('control', '')) or re.fullmatch(r'(Param|Result)\d+', text):
                continue
            # (a test asset's members in a graph's menu: DA_Test.Damage, E_Test::NewEnumerator0; a variable's getter or
            # setter, "Get Blip": the user's own names)
            # (and a variable's own name, one CamelCase word: IsInAir)
            if (re.search(r'_Test[.:]|Test::', text) or re.fullmatch(r'(Get|Set) [A-Za-z_][A-Za-z0-9_]*', text) or
                    re.fullmatch(r'[A-Z][a-z0-9]+(?:[A-Z][a-z0-9]*)+', text)):
                continue
            if text in back:  # (a translation: only kept when the code passes it to TR as a literal)
                if back[text] == text:  # ("C++": the same in every language)
                    continue
                where = literal_in_code(files, text)
                if not where:
                    continue
                p = 'literal em pt passado ao TR: "%s" (%s)' % (text, where)
        parts.append(p)
    return parts

def main():
    if len(sys.argv) < 2:
        print('usage: ui_sweep_report.py <dir> [--src src]')
        return 2
    d = sys.argv[1]
    src = sys.argv[sys.argv.index('--src') + 1] if '--src' in sys.argv else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src')
    recs = []
    p = os.path.join(d, 'ui_sweep.jsonl')
    for line in io.open(p, encoding='utf-8', errors='replace'):
        line = line.strip()
        if line:
            try:
                recs.append(json.loads(line))
            except ValueError:
                pass
    back, files = load_tables(src), index_sources(src)
    for r in recs:  # (the false misses dropped: a control left with no problem is ok)
        if r.get('result') == 'fail':
            r['problems'] = '; '.join(clean(r, back, files))
            if not r['problems']:
                r['result'] = 'ok'
    fails = [r for r in recs if r.get('result') == 'fail']
    running = os.path.join(d, 'ui_sweep_running.txt')
    crashed = io.open(running, encoding='utf-8').read().strip() if os.path.exists(running) else ''
    with io.open(os.path.join(d, 'ui_sweep.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Varredura da interface (--ui-sweep)\n\n')
        f.write('- %d controles, %d com problema%s\n\n' % (len(recs), len(fails), '; **a execução parou (crash?) em: %s**' % crashed if crashed else ''))
        f.write('| Controle | Resultado | Pior quadro (ms) | Problemas | Última linha do log |\n|---|---|---|---|---|\n')
        for r in recs:
            f.write('| %s | %s | %.0f | %s | %s |\n' % (r['control'].replace('|', '/'), r['result'], r.get('worst_ms', 0),
                                                     r.get('problems', '').replace('|', '/')[:300], r.get('log', '').replace('|', '/')[:120]))
    with io.open(os.path.join(d, 'ui_sweep_bugs.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Bugs da varredura da interface (um por linha: o controle, o erro, o arquivo provável)\n\n')
        if crashed:
            f.write('- `%s`: a execução parou aqui (crash ou travada) — arquivo provável: %s\n' % (crashed, probable_file({'control': crashed, 'problems': ''}, back, files)))
        groups = {}  # one line per problem: the controls where it showed
        for r in fails:
            for p in r['problems'].split('; '):
                key = re.sub(r'\d+ validation error\(s\)', 'validation error', p)
                g = groups.setdefault(key, {'controls': []})
                g['controls'].append(r['control'])
        for key, g in groups.items():
            more = ' (e mais %d controles)' % (len(g['controls']) - 1) if len(g['controls']) > 1 else ''
            f.write('- `%s`%s: %s — arquivo provável: %s\n' % (g['controls'][0], more, key[:400],
                                                                probable_file({'control': g['controls'][0], 'problems': key}, back, files)))
    groups = groups if crashed or fails else {}
    print('ui_sweep_report: %d controls, %d with problems, %d bugs%s -> %s' % (len(recs), len(fails), len(groups), ' (stopped at: %s)' % crashed if crashed else '',
                                                                     os.path.join(d, 'ui_sweep_bugs.md')))
    if '--check' in sys.argv:  # (2026-11-25, item 22: the suite's ui_sweep_clean) any bug fails, a few shown
        if not recs:
            print('FAIL: ui_sweep_clean: no ui_sweep.jsonl results in %s (did the ui_sweep test run?)' % d)
            return 1
        if crashed:
            print('FAIL: ui_sweep_clean: the sweep stopped at %s' % crashed)
        for key, g in list(groups.items())[:15]:
            print('FAIL: ui_sweep_clean: %s: %s' % (g['controls'][0], key[:200]))
        if crashed or fails:
            return 1
        print('ui_sweep_clean: OK (%d controls, no problem)' % len(recs))
    return 0

if __name__ == '__main__':
    sys.exit(main())
