#!/usr/bin/env python3
"""(Q16, notas-dev/em-jogo.md) The Details' properties made editable in the game.

Reads the Details panel's controls (src/editor.c's row_float / row_slider / row_check / row_color / vec3_row, and the
igDragFloat / igSliderFloat / igCheckbox / igColorEdit3 / igDragInt / igSliderInt of the components' own panels in
src/*_ui.c), resolves each one's pointer to a component's field (the struct definitions of src/*.h, ecs.c's
g_components for the component of a struct) and writes src/props_table.inc: one P(...) line a property, read by
src/props.c (Set / Get Property by Name in the Blueprints, the game included).

    python tools/gen_props.py          # writes src/props_table.inc
    python tools/gen_props.py --check  # exit 1 when src/props_table.inc is not what the sources give (the test)

tools/ingame_sweep.py uses the same parse to list what stays "editor only".
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src')
OUT = os.path.join(SRC, 'props_table.inc')

# the files whose controls are the Details' (the components' panels)
DETAIL_FILES = ['editor.c', 'editor_spline.inc', 'fpcam_ui.c', 'interaction_ui.c', 'rules_ui.c', 'inventory_ui.c', 'ability_ui.c', 'pcg_editor.c']

KINDS = {  # C type -> (kind, bytes)
    'float': ('PK_F32', 4), 'bool': ('PK_BOOL', 1), 'uint32_t': ('PK_U32', 4), 'int32_t': ('PK_I32', 4), 'int': ('PK_I32', 4),
    'unsigned': ('PK_U32', 4), 'uint8_t': ('PK_U8', 1), 'int8_t': ('PK_U8', 1), 'uint16_t': ('PK_U16', 2), 'char': ('PK_STR', 1),
}
HELPERS = {  # name -> (label arg, pointer arg, lo arg, hi arg)
    'row_float': (0, 1, 3, 4), 'row_slider': (0, 1, 2, 3), 'row_check': (0, 1, None, None), 'row_color': (0, 1, None, None),
    'vec3_row': (0, 1, None, None),
    'igDragFloat': (0, 1, 3, 4), 'igSliderFloat': (0, 1, 2, 3), 'igCheckbox': (0, 1, None, None), 'igColorEdit3': (0, 1, None, None),
    'igColorEdit4': (0, 1, None, None), 'igDragInt': (0, 1, 3, 4), 'igSliderInt': (0, 1, 2, 3), 'igDragFloat3': (0, 1, 3, 4),
    'igSliderFloat3': (0, 1, 2, 3),
    'flag': (0, 1, None, None), 'flag32': (0, 1, None, None), 'part': (0, 1, None, None),  # a bit of a field (mask: 3rd)
}
BIT_HELPERS = {'flag': 2, 'flag32': 2, 'part': 2}  # the mask's argument
# checkboxes that copy a bit (or a 0/1 field) through a local: (function, label) -> (pointer->field, mask, invert)
EXTRA_BITS = {
    ('details', 'Show Icon'): ('t->flags', 'TRANSFORM_NO_ICON', 1),
    ('details', 'Collision Enabled'): ('t->flags', 'TRANSFORM_NO_COLLISION', 1),
    ('constraint_6dof_rows', 'Soft Linear Limit'): ('pc->soft_limits', '1u', 0),
    ('constraint_6dof_rows', 'Soft Angular Limit'): ('pc->soft_limits', '2u', 0),
    ('constraint_6dof_rows', 'Linear Position Drive'): ('pc->lin_drive', 'CDRIVE_POSITION', 0),
    ('constraint_6dof_rows', 'Linear Velocity Drive'): ('pc->lin_drive', 'CDRIVE_VELOCITY', 0),
    ('constraint_6dof_rows', 'Angular Orientation Drive'): ('pc->ang_drive', 'CDRIVE_POSITION', 0),
    ('constraint_6dof_rows', 'Angular Velocity Drive'): ('pc->ang_drive', 'CDRIVE_VELOCITY', 0),
    ('details', 'Infinite Extent (Unbound)'): ('ppv->flags', 'PPV_UNBOUND', 0),
    ('details', 'Enabled', '&en'): ('ppv->flags', 'PPV_ENABLED', 0),  # (two "Enabled" there: this one by its pointer)
    ('details', 'Horror Exposure'): ('ppv->terror_flags', 'TERROR_PPV_EXPOSURE', 0),
    ('details', 'Tension Curves'): ('ppv->terror_flags', 'TERROR_PPV_TENSION', 0),
    ('details', 'Camera Motion Blur'): ('cam->no_camera_blur', '1u', 1),
    ('details', 'Disable Collision'): ('pc->keep_collision', '1u', 1),
    ('details', 'LOD'): ('grm->lod', '1u', 0),
    ('details', 'Dynamic'): ('grm->dynamic', '1u', 0),
    ('details', 'Collide'): ('grm->collide', '1u', 0),
    ('details', 'Visible', '&gvis'): ('grm->visible', '1u', 0),  # (2026-11-25, item 16: the groom on / off)
    ('fpcam_ui', 'Use First Person Camera'): ('c->fp', 'FP_ON', 0),
    ('ia_ui_interactable', 'Can Repeat'): ('c->flags', 'IA_ONCE', 1),
    ('abil_ui_system', 'Activate with Keys 1-6'): ('c->flags', 'ABIL_KEYS', 0),
    ('abil_ui_system', 'Show Ability Bar'): ('c->flags', 'ABIL_SHOW_BAR', 0),
}
HELPER_DEFS = re.compile(r'^(row_\w+|vec3_row|\w*flag\w*|label_cell|part)$')  # the helpers' own bodies
CALL = re.compile(r'\b(' + '|'.join(sorted(HELPERS, key=len, reverse=True)) + r')\s*\(')


def strip_comments(t):
    t = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), t, flags=re.S)
    return re.sub(r'//[^\n]*', '', t)


def read(p):
    return open(p, encoding='utf-8', errors='replace').read()


def parse_structs():
    """{struct name: {field: (ctype, dims)}} of every typedef struct in src/*.h; the enum typedefs' names"""
    structs, enums = {}, set()
    for f in sorted(os.listdir(SRC)):
        if not f.endswith('.h'):
            continue
        t = strip_comments(read(os.path.join(SRC, f)))
        for m in re.finditer(r'typedef\s+enum\s*\w*\s*\{[^}]*\}\s*(\w+)\s*;', t):
            enums.add(m.group(1))
        for m in re.finditer(r'typedef\s+struct\s+(\w+)?\s*\{', t):
            i, depth = m.end(), 1
            while i < len(t) and depth:
                depth += {'{': 1, '}': -1}.get(t[i], 0)
                i += 1
            body = t[m.end():i - 1]
            tail = re.match(r'\s*(\w+)\s*;', t[i:])
            name = tail.group(1) if tail else m.group(1)
            if not name:
                continue
            fields = {}
            body = re.sub(r'\{[^{}]*\}', '', body)  # (an inner anonymous struct / union: left out)
            for st in body.split(';'):
                st = ' '.join(st.split())
                st = re.sub(r'\b(const|volatile|unsigned\s+(?=int))\s*', '', st)
                mm = re.match(r'(?:struct\s+)?([A-Za-z_]\w*)\s+(.+)$', st)
                if not mm or ':' in st:
                    continue
                ctype = mm.group(1)
                for d in mm.group(2).split(','):
                    d = d.strip()
                    dm = re.match(r'(\**)\s*([A-Za-z_]\w*)\s*((?:\[[^\]]*\])*)$', d)
                    if not dm or dm.group(1):
                        continue
                    dims = re.findall(r'\[([^\]]*)\]', dm.group(3))
                    fields[dm.group(2)] = (ctype, dims)
            structs[name] = fields
    return structs, enums


def comp_of_struct():
    """{struct name: COMP_ID} from ecs.c's g_components"""
    t = read(os.path.join(SRC, 'ecs.c'))
    return {m.group(2): m.group(1) for m in re.finditer(r'\[(COMP_\w+)\]\s*=\s*\{\s*"[^"]*"\s*,\s*sizeof\((\w+)\)', t)}


def split_args(t, i):
    """the arguments of the call whose '(' is at t[i]; (args, end)"""
    depth, args, cur, j, instr = 0, [], '', i, False
    while j < len(t):
        c = t[j]
        if instr:
            cur += c
            if c == '\\':
                cur += t[j + 1]
                j += 2
                continue
            if c == '"':
                instr = False
        elif c == '"':
            instr = True
            cur += c
        elif c == '(':
            depth += 1
            if depth > 1:
                cur += c
        elif c == ')':
            depth -= 1
            if depth == 0:
                args.append(cur.strip())
                return args, j
            cur += c
        elif c == ',' and depth == 1:
            args.append(cur.strip())
            cur = ''
        else:
            cur += c
        j += 1
    return args, j


def label_of(a):
    m = re.search(r'"((?:[^"\\]|\\.)*)"', a)
    if not m:
        return None
    s = m.group(1).split('##')[0]
    return s if s.strip() else None


def num(a):
    if a is None:
        return None
    a = a.strip()
    m = re.fullmatch(r'-?\d+(?:\.\d*)?(?:e-?\d+)?f?', a)
    return float(a.rstrip('f')) if m else None


def resolve(structs, enums, ctype, path):
    """the field's kind and count along path ("a.b", "c[2]"); None with a reason"""
    parts = path.split('.')
    t = ctype
    for k, p in enumerate(parts):
        pm = re.fullmatch(r'(\w+)((?:\[[^\]]*\])*)', p)
        if not pm or t not in structs:
            return None, 'not a field of a known struct'
        name, idx = pm.group(1), re.findall(r'\[([^\]]*)\]', pm.group(2))
        if name not in structs[t]:
            return None, 'no field %s in %s' % (name, t)
        ft, dims = structs[t][name]
        if any(not re.fullmatch(r'\d+', i) for i in idx):
            return None, 'indexed by a variable'
        rest = dims[len(idx):]
        if k < len(parts) - 1:
            if rest:
                return None, 'an array of structs'
            t = ft
            continue
        if ft in enums:
            ft = 'int'
        if ft not in KINDS:
            return None, 'a field of type %s' % ft
        n = 1
        for d in rest:
            if not re.fullmatch(r'\d+', d.strip()):
                return None, 'an array of a named size'
            n *= int(d)
        return (KINDS[ft][0], n), None
    return None, 'empty'


def details_controls():
    """every Details control: dict(file, line, func, label, section, ptr, comp, struct, field, kind, count, lo, hi, why)"""
    structs, enums = parse_structs()
    comps = comp_of_struct()
    out = []
    for f in DETAIL_FILES:
        p = os.path.join(SRC, f)
        if not os.path.exists(p):
            continue
        t = strip_comments(read(p))
        line_at = [0]
        for c in t:
            line_at.append(line_at[-1] + (c == '\n'))
        # the declarations "T* v" (locals and parameters), in order: the last one before a control is its
        decls = [(m.start(), m.group(2), m.group(1)) for m in re.finditer(r'\b([A-Z]\w*)\s*\*\s*(\w+)\s*(?=[=,)])', t)
                 if m.group(1) in structs]
        funcs = [(m.start(), m.group(1)) for m in re.finditer(r'^\w[^\n;]*?\b(\w+)\s*\([^;{]*\)\s*\{', t, flags=re.M)]
        sections = [(m.start(), m.group(1)) for m in re.finditer(r'begin_section\(\s*"([^"]+)"', t)]
        for m in CALL.finditer(t):
            helper = m.group(1)
            args, _ = split_args(t, m.end() - 1)
            la, pa, loa, hia = HELPERS[helper]
            if len(args) <= pa:
                continue
            pos = m.start()
            fn = ([n for s, n in funcs if s <= pos] or ['?'])[-1]
            fstart = ([s for s, n in funcs if s <= pos] or [0])[-1]
            sec = ([n for s, n in sections if fstart <= s <= pos] or [None])[-1]
            if HELPER_DEFS.match(fn):  # the helpers' own bodies: not controls
                continue
            lab = label_of(args[la])
            if not lab and '"##' in args[la]:  # an id only: the row's label is the label_cell just before it
                lc = list(re.finditer(r'label_cell\(\s*"([^"]+)"', t[max(fstart, pos - 400):pos]))
                lab = lc[-1].group(1) if lc else None
            rec = dict(file='src/' + f, line=line_at[pos] + 1, func=fn, helper=helper, label=lab, section=sec,
                       ptr=args[pa], comp=None, struct=None, field=None, kind=None, count=0, why=None,
                       lo=num(args[loa]) if loa is not None and loa < len(args) else None,
                       hi=num(args[hia]) if hia is not None and hia < len(args) else None)
            out.append(rec)
            if not rec['label']:
                rec['why'] = 'its label is not a literal'
                continue
            pm = re.fullmatch(r'&?\s*\(?\s*(\w+)\s*->\s*([\w.\[\]]+)\s*\)?', args[pa])
            var = path = None
            if pm:
                var, path = pm.group(1), pm.group(2)
            lm = re.fullmatch(r'&\s*(\w+)', args[pa].strip())
            if not pm and lm:  # a local copied straight back into a field: if (row(..., &x, ...)) v->field = (cast)x
                _, end = split_args(t, m.end() - 1)
                cm = re.match(r'\s*\)\s*\{?\s*(\w+)\s*->\s*([\w.\[\]]+)\s*=\s*(?:\(\w+\)\s*)?' + re.escape(lm.group(1)) + r'\s*[,;]',
                              t[end + 1:end + 200])
                if cm:
                    var, path = cm.group(1), cm.group(2)
            mask, inv = None, 0
            if helper in BIT_HELPERS and len(args) > BIT_HELPERS[helper]:
                mask = args[BIT_HELPERS[helper]]
            xk = (fn, lab, args[pa].strip()) if (fn, lab, args[pa].strip()) in EXTRA_BITS else (fn, lab)
            if xk in EXTRA_BITS and not pm:  # a bit copied through a local (the hand list above)
                expr, mask, inv = EXTRA_BITS[xk]
                var, path = expr.split('->')
            if not var:
                rec['why'] = 'a local value (%s), converted into the component by hand' % args[pa]
                continue
            ds = [d for d in decls if d[1] == var and d[0] <= pos]
            if not ds:
                rec['why'] = 'the pointer %s has no declared type here' % var
                continue
            st = ds[-1][2]
            # the struct's component: itself, or the component that holds it (a nested settings struct)
            comp, prefix = comps.get(st), ''
            if not comp:
                for cs, cid in comps.items():
                    for fname, (ft, dims) in structs.get(cs, {}).items():
                        if ft == st and not dims:
                            comp, prefix, st = cid, fname + '.', cs
                            break
                    if comp:
                        break
            if not comp:
                rec['why'] = 'the struct %s is not a component' % st
                continue
            r, why = resolve(structs, enums, st, prefix + path)
            if not r:
                rec['why'] = why
                continue
            rec['comp'], rec['struct'], rec['field'], (rec['kind'], rec['count']) = comp, st, prefix + path, r
            if mask:  # a bit of the field (its bytes: 2 or 4)
                if rec['kind'] not in ('PK_U32', 'PK_I32', 'PK_U16', 'PK_FLAG') or rec['count'] != 1:
                    rec['why'] = 'a bit of a field of kind %s' % rec['kind']
                    rec['comp'] = None
                    continue
                rec['bytes'] = 2 if rec['kind'] == 'PK_U16' else 4
                rec['kind'], rec['mask'], rec['inv'] = 'PK_BIT', mask, inv
            elif helper in ('row_check', 'igCheckbox') and rec['kind'] in ('PK_U32', 'PK_I32'):
                rec['kind'] = 'PK_FLAG'  # a uint32 shown as a checkbox: 0 / 1
    return out


def table(rows):
    seen, lines = set(), []
    for r in rows:
        if r['why'] or not r['comp']:
            continue
        key = (r['comp'], r['field'], r.get('mask'))
        if key in seen:
            continue
        seen.add(key)
        lab = r['label'].replace('\\', '\\\\').replace('"', '\\"')
        sec = (r['section'] or '').replace('"', '\\"')
        if r['kind'] == 'PK_BIT':
            lines.append('B(%s, %s, "%s", "%s", %s, %s, %d, %d) // %s %s()' % (r['comp'], r['struct'], sec, lab, r['field'], r['mask'],
                                                                         r['inv'], r['bytes'], r['file'], r['func']))
            continue
        lo = r['lo'] if r['lo'] is not None else 0.0
        hi = r['hi'] if r['hi'] is not None else 0.0
        lines.append('P(%s, %s, "%s", "%s", %s, %s, %d, %sf, %sf) // %s %s()' % (r['comp'], r['struct'], sec, lab, r['field'], r['kind'],
                                                                            r['count'], repr(float(lo)), repr(float(hi)), r['file'], r['func']))
    return lines


def render(rows):
    t = table(rows)
    head = ['// Generated by tools/gen_props.py from the Details panels (src/editor.c, src/*_ui.c): do not edit.',
            '// P(component, struct, section, label, field, kind, count, lo, hi); B(component, struct, section, label, field, mask, invert,',
            '// bytes): a checkbox that is a bit of a field. props.c reads it (Set / Get Property by Name).',
            '// %d properties.' % len(t)]
    return '\n'.join(head + t) + '\n'


def main():
    rows = details_controls()
    text = render(rows)
    if '--check' in sys.argv:
        cur = read(OUT) if os.path.exists(OUT) else ''
        if cur != text:
            print('gen_props: src/props_table.inc is stale: run python tools/gen_props.py')
            return 1
        print('gen_props: OK (%d properties)' % (text.count('\nP(') + text.count('\nB(')))
        return 0
    open(OUT, 'w', encoding='utf-8', newline='\n').write(text)
    print('gen_props: %d properties into %s' % (text.count('\nP(') + text.count('\nB('), os.path.relpath(OUT, ROOT)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
