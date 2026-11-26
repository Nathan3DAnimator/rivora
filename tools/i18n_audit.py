# The i18n_audit test (ctest, area editor; notas-dev/idiomas.md): the editor's visible texts all go through TR().
#   1. Every string literal shown by a widget or a drawing call (igText*, igButton, igMenuItem, igBegin, tooltips, table
#      headers, ImDrawList_AddText, ui_menu_item, editor_status, the graphs' labels...) that holds words and isn't wrapped
#      in TR()/TR_ID()/N_() is reported, unless every word in it is an exception (names, units, Unreal's terms kept in
#      English: tools/i18n_exceptions.txt) or the line says "i18n: none (<why>)".
#   2. Every TR("...") key of src/ must be in the table with its Portuguese and Spanish texts (i18n_table.c's pt,
#      i18n_table_es.c's es).
# The literals already there in the files other sessions own are listed in tools/i18n_baseline.txt (file and text): those
# are reported as "known" and don't fail; a new one fails.
#   python tools/i18n_audit.py [src_dir] [--list] [--write-baseline] [--keys]
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TEXT_CALLS = (
    # a widget's label (its first literal) or a text drawn
    r'igText|igTextUnformatted|igTextColored|igTextDisabled|igTextWrapped|igBulletText|igLabelText|igSeparatorText|'
    r'igSetTooltip|igSetItemTooltip|igBegin|igBeginPopupModal|igOpenPopup_Str|igBeginPopup|igTableSetupColumn|igButton|'
    r'igSmallButton|igCheckbox|igCheckboxFlags_IntPtr|igRadioButton_Bool|igRadioButton_IntPtr|igSelectable_Bool|'
    r'igSelectable_BoolPtr|igMenuItem_Bool|igMenuItem_BoolPtr|igBeginMenu|igBeginTabItem|igCollapsingHeader_TreeNodeFlags|'
    r'igCollapsingHeader_BoolPtr|igTreeNodeEx_Str|igTreeNode_Str|igBeginCombo|igCombo_Str_arr|igCombo_Str|igSliderFloat|'
    r'igSliderFloat2|igSliderFloat3|igSliderInt|igSliderAngle|igDragFloat|igDragFloat2|igDragFloat3|igDragFloat4|'
    r'igDragFloatRange2|igDragInt|igDragInt2|igDragIntRange2|igInputFloat|igInputFloat2|igInputFloat3|igInputInt|igInputText|'
    r'igInputTextWithHint|igInputTextMultiline|igColorEdit3|igColorEdit4|igInputDouble|igPlotLines_FloatPtr|'
    r'igPlotHistogram_FloatPtr|igProgressBar|ImDrawList_AddText_Vec2|ImDrawList_AddText_FontPtr|ui_menu_item|'
    r'ui_menu_item_ptr|ui_begin_menu|ui_icon_button|ui_dropdown_button|editor_status|ui_tip_item|ui_num_float|ui_num_int|'
    r'ui_slider_float|ui_slider_int|ui_num_float_range|ui_num_float3|kit_hud|content_message|async_status|async_fail')
CALL = re.compile(r'\b(' + TEXT_CALLS + r')\(')
WRAP = re.compile(r'\b(TR|TR_ID|N_|tr|tr_id|tr_lang|PL)\(\s*$')
TRKEY = re.compile(r'\b(?:TR|TR_ID|N_|tr|tr_id)\(\s*"((?:[^"\\]|\\.)*)"\s*\)')
SKIP_FILES = ('ui_widget_draw.c', 'hud_widget.c', 'inventory_widget.c', 'quest_hud.c', 'ability_widget.c',  # the game's UI
              'i18n_table.c', 'i18n_table_es.c', 'i18n.c')
WORD = re.compile(r"[A-Za-z][A-Za-z'\-]*")
FMT = re.compile(r'%[-+ #0]*\d*(?:\.\d+)?(?:ll|l|z|h)?[diouxXeEfgGcsp%]')

def unescape(s):
    s = re.sub(r'((?:\\x[0-9a-fA-F]{2})+)', lambda m: bytes.fromhex(m.group(1).replace('\\x', '')).decode('utf-8', 'replace'), s)
    return s.replace('\\"', '"').replace('\\\\', '\\').replace('\\n', '\n')

def exceptions():
    words = set()
    p = os.path.join(HERE, 'i18n_exceptions.txt')
    if os.path.exists(p):
        for line in open(p, encoding='utf-8'):
            line = line.split('#', 1)[0].strip()
            if line:
                words.add(line.lower())
    return words

def visible_part(lit):
    vis = lit.split('##', 1)[0]
    return FMT.sub(' ', vis)

def needs_tr(lit, exc):
    vis = visible_part(lit)
    if not re.search(r'\s', vis.strip()) and (re.search(r'[/:]', vis) or re.search(r'[A-Za-z0-9]\.[A-Za-z]', vis)):  # a tag, a path, a URL: data
        return False
    if vis.strip().lower() in exc:
        return False
    words = WORD.findall(vis)
    words = [w for w in words if len(w) >= 2]
    if not words:
        return False
    return any(w.lower() not in exc for w in words)

def call_args(text, start):
    """The text between the call's parentheses (start: just after '('); strings and nesting skipped."""
    depth, i, n = 1, start, len(text)
    while i < n and depth:
        c = text[i]
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == '\\' else 1
        elif c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        i += 1
    return text[start:i - 1]

LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')

# the arguments that are ids or shortcuts, not text shown: (call, argument index)
SKIP_ARGS = {'igOpenPopup_Str': (0,), 'igBeginPopup': (0,), 'ui_icon_button': (0,), 'ui_dropdown_button': (0,),
             'igMenuItem_Bool': (1,), 'igMenuItem_BoolPtr': (1,), 'ui_menu_item': (2,), 'ui_menu_item_ptr': (2,),
             'ui_tip_item': (3,), 'igBeginPopupContextItem': (0,), 'igBeginChild_Str': (0,)}

def split_args(args):
    """The top-level arguments with their start offsets."""
    out, depth, i, n, start = [], 0, 0, len(args), 0
    while i < n:
        c = args[i]
        if c == '"':
            i += 1
            while i < n and args[i] != '"':
                i += 2 if args[i] == '\\' else 1
        elif c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        elif c == ',' and depth == 0:
            out.append((start, args[start:i]))
            start = i + 1
        i += 1
    out.append((start, args[start:]))
    return out

def scan_file(path, exc):
    text = open(path, encoding='utf-8', errors='replace').read()
    lines = text.split('\n')
    found = []
    for m in CALL.finditer(text):
        line_no = text.count('\n', 0, m.start()) + 1
        if 'i18n: none' in lines[line_no - 1]:
            continue
        args = call_args(text, m.end())
        skip = SKIP_ARGS.get(m.group(1), ())
        for k, (off, arg) in enumerate(split_args(args)):
            if k in skip or re.match(r'\s*(?:PL\(\s*)?(?:TR|TR_ID|N_|tr|tr_id)\(', arg):  # (a TR("a" "b") whole)
                continue
            for lm in LITERAL.finditer(arg):
                before = arg[:lm.start()]
                if WRAP.search(before):  # TR("...")
                    continue
                lit = unescape(lm.group(1))
                if m.group(1) == 'igTableSetupColumn' and re.fullmatch(r'[a-z_]+', lit):  # (a column's id: no headers row)
                    continue
                if needs_tr(lit, exc):
                    found.append((line_no, lit))
    return found

def tr_keys(root):
    keys = {}
    for f in sorted(os.listdir(root)):
        if not (f.endswith('.c') or f.endswith('.inc') or f.endswith('.h')) or f in SKIP_FILES or f.startswith('ui_tips_table'):
            continue
        text = open(os.path.join(root, f), encoding='utf-8', errors='replace').read()
        for m in TRKEY.finditer(text):
            k = unescape(m.group(1))
            if '##' in k:
                k = k.split('##', 1)[0]
            if k and re.search(r'[A-Za-z]', k):
                keys.setdefault(k, f)
    return keys

ENTRY = re.compile(r'\{\s*((?:"(?:[^"\\]|\\.)*"\s*)+),\s*\{\s*((?:"(?:[^"\\]|\\.)*"\s*)+|NULL)?')
def joined(lits):
    """Adjacent literals as C joins them ("a" "b": "ab")."""
    return ''.join(m.group(1) for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', lits or ''))
ES_ENTRY = re.compile(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}')

def table(root):
    pt, es = {}, {}
    for f in os.listdir(root):
        if f.startswith('i18n_table') and f.endswith('.c') and f != 'i18n_table_es.c':
            for m in ENTRY.finditer(open(os.path.join(root, f), encoding='utf-8').read()):
                pt[unescape(joined(m.group(1)))] = unescape(joined(m.group(2)))
    p = os.path.join(root, 'i18n_table_es.c')
    if os.path.exists(p):
        for m in ES_ENTRY.finditer(open(p, encoding='utf-8').read()):
            es[unescape(m.group(1))] = unescape(m.group(2))
    return pt, es

def dumped(exe):
    """The tables' texts (rivora --i18n-dump): {text: kind}."""
    import subprocess, tempfile
    out = {}
    if not exe:
        return out
    path = os.path.join(tempfile.gettempdir(), 'rivora_i18n_dump.txt')
    subprocess.run([exe, '--i18n-dump', path], check=False, capture_output=True)
    if os.path.exists(path):
        for line in open(path, encoding='utf-8', errors='replace'):
            parts = line.rstrip('\n').split('\t')
            kind, text = parts[0], parts[1] if len(parts) > 1 else ''
            # (the nodes' descriptions: their tooltips, translated when the table has them; a material option: only its
            # Portuguese, its English in mat_i18n.c)
            if text and kind not in ('node_tip', 'mat_prop', 'mat_item'):
                out.setdefault(text, kind)
    return out

def main():
    exe = None
    if '--exe' in sys.argv:
        exe = sys.argv[sys.argv.index('--exe') + 1]
    args = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith('--') and sys.argv[i - 1] != '--exe']
    root = args[0] if args else os.path.join(HERE, '..', 'src')
    exc = exceptions()
    base_path = os.path.join(HERE, 'i18n_baseline.txt')
    baseline = set()
    if os.path.exists(base_path):
        for line in open(base_path, encoding='utf-8'):
            line = line.rstrip('\n')
            if line and not line.startswith('#'):
                baseline.add(line)
    new, known, everything = [], 0, []
    for f in sorted(os.listdir(root)):
        if not (f.endswith('.c') or f.endswith('.inc')) or f in SKIP_FILES or f.startswith('ui_tips_table') or f.endswith('_test.c') \
                or f.endswith('_test.inc') or f.startswith('test_'):
            continue
        for line_no, lit in scan_file(os.path.join(root, f), exc):
            key = '%s\t%s' % (f, lit.replace('\n', '\\n'))
            everything.append(key)
            if key in baseline:
                known += 1
            else:
                new.append('%s:%d  "%s"' % (f, line_no, lit.replace('\n', '\\n')))
    if '--write-baseline' in sys.argv:
        with open(base_path, 'w', encoding='utf-8', newline='\n') as o:
            o.write('# i18n_audit: visible literals without TR() still in files other sessions own (file<TAB>text). Fix them and\n'
                    '# delete the line; never add a line for new code.\n')
            for k in sorted(set(everything)):
                o.write(k + '\n')
        print('baseline: %d literals' % len(set(everything)))
        return 0
    keys = tr_keys(root)
    for text, kind in dumped(exe).items():
        keys.setdefault(text, 'table:' + kind)
    pt, es = table(root)
    keys = {k: v for k, v in keys.items() if needs_tr(k, exc)}  # (a key of only names and units: nothing to translate)
    no_pt = sorted(k for k in keys if not pt.get(k))
    no_es = sorted(k for k in set(keys) | set(pt) if not es.get(k) and needs_tr(k, exc))
    fail = bool(new) or bool(no_pt) or bool(no_es)
    if '--keys' in sys.argv:
        for k in no_pt:
            print('PT\t%s\t%s' % (keys[k], k.replace('\n', '\\n')))
        for k in no_es:
            print('ES\t%s' % k.replace('\n', '\\n'))
        return 0
    if new:
        print('FAIL: %d visible literal(s) without TR():' % len(new))
        for x in new[: (100000 if '--list' in sys.argv else 40)]:
            print('  ' + x)
    if no_pt:
        print('FAIL: %d TR key(s) without Portuguese:' % len(no_pt))
        for k in no_pt[: (100000 if '--list' in sys.argv else 20)]:
            print('  "%s"  (%s)' % (k, keys[k]))
    if no_es:
        print('FAIL: %d key(s) without Spanish (i18n_table_es.c)' % len(no_es))
        for k in no_es[: (100000 if '--list' in sys.argv else 10)]:
            print('  "%s"' % k)
    if not fail:
        print('i18n_audit: OK, %d TR keys in pt and es, %d known literals left in other owners\' files' % (len(keys), known))
    return 1 if fail else 0

if __name__ == '__main__':
    sys.exit(main())
