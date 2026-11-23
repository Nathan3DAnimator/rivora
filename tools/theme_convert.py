# The one-time conversion of the editor's fixed colors into theme tokens (Q1, notas-dev/temas.md).
#   python tools/theme_convert.py [--apply] [files...]   (default: every src/*.c but theme.c; without --apply: a report)
# Each literal color (0xAABBGGRR, IM_COL32(r, g, b[, a]), (ImVec4){ r, g, b, a }) becomes the token of the same family
# with the same alpha: grays -> the neutral ramp TK_N0-TK_N15 (by lightness, the anchors of theme.c's g_ramp_at),
# the old Unreal blue -> TK_ACCENT*, colored texts (igTextColored) -> the states, the rest -> TK_<HUE>[_D|_L] (the
# centers of theme.c's g_hue_at). A light theme turns the ramp around, so the old darkest gray becomes its lightest.
# Kept: bit masks (& | ^ << >>), image tints (white in an Image call), colors a user picks (data), file-scope tables
# (reported: they need TK_ arrays by hand).
import colorsys, os, re, sys

RAMP_AT = [0, 10, 16, 21, 26, 32, 38, 46, 56, 66, 90, 115, 145, 180, 215, 250]
HUES = ['RED', 'ORANGE', 'AMBER', 'LIME', 'GREEN', 'TEAL', 'CYAN', 'BLUE', 'VIOLET', 'MAGENTA', 'PINK']
HUE_AT = [0, 25, 45, 85, 135, 168, 192, 215, 262, 295, 330]
ACCENT = [((0, 112, 224), 'TK_ACCENT'), ((0, 140, 255), 'TK_ACCENT'), ((30, 140, 255), 'TK_ACCENT'), ((30, 140, 224), 'TK_ACCENT'),
          ((0, 90, 180), 'TK_ACCENT_STRONG'), ((0, 77, 158), 'TK_ACCENT_DIM'), ((18, 74, 107), 'TK_ACCENT_DIM')]

def classify(r, g, b, text_ctx=False):
    for (ar, ag, ab), tok in ACCENT:
        if (r - ar) ** 2 + (g - ag) ** 2 + (b - ab) ** 2 <= 22 ** 2:
            return tok
    h, l, s = colorsys.rgb_to_hls(r / 255, g / 255, b / 255)
    if max(r, g, b) - min(r, g, b) <= 14 or s < 0.12:
        v = (r + g + b) / 3
        i = min(range(16), key=lambda k: abs(RAMP_AT[k] - v))
        return 'TK_N%d' % i
    deg = h * 360
    best = min(range(11), key=lambda k: min(abs(deg - HUE_AT[k]), 360 - abs(deg - HUE_AT[k])))
    if text_ctx:  # a message's color: its state
        name = HUES[best]
        if name in ('RED', 'PINK', 'MAGENTA'): return 'TK_ERROR'
        if name in ('ORANGE', 'AMBER'): return 'TK_WARN'
        if name in ('LIME', 'GREEN', 'TEAL'): return 'TK_OK'
        return 'TK_INFO'
    tone = '_D' if l < 0.38 else '_L' if l > 0.66 else ''
    return 'TK_%s%s' % (HUES[best], tone)

HEX = re.compile(r'0x([0-9A-Fa-f]{8})u?\b')
COL = re.compile(r'IM_COL32\(\s*([^,()]+?)\s*,\s*([^,()]+?)\s*,\s*([^,()]+?)\s*(?:,\s*((?:[^()]|\([^()]*\))+?)\s*)?\)')
V4 = re.compile(r'\(ImVec4\)\{\s*([0-9.]+)f?\s*,\s*([0-9.]+)f?\s*,\s*([0-9.]+)f?\s*,\s*([0-9.]+)f?\s*\}')
NUM = re.compile(r'^\d+$')
BITCTX = re.compile(r'(&|\||\^|<<|>>|~)\s*$')
IMAGE = re.compile(r'Image|AddImage|igImage|ImageButton|tint', re.I)

def tok_expr(tok, a, v4=False):
    if v4:
        return 'THV(%s)' % tok if a >= 0.999 else 'THVA(%s, %sf)' % (tok, ('%.3f' % a).rstrip('0').rstrip('.') if a > 0 else '0')
    if isinstance(a, str): return 'THA(%s, %s)' % (tok, a)
    return 'TH(%s)' % tok if a == 255 else 'THA(%s, %d)' % (tok, a)

def convert_line(line, report, fn, ln, at_file_scope):
    if 'theme: literal' in line:  # (a color that is data: kept, the test skips it too)
        return line
    code, sep, comment = line.partition('//')
    if '"' in code:  # (strings: never inside one)
        pass
    text_ctx = 'TextColored' in code or 'ImGuiCol_Text,' in code and 'Push' in code
    out = code
    def hex_sub(m):
        pre = out[:m.start()]
        if BITCTX.search(pre) or re.match(r'\s*(\||&|\^|<<|>>)', out[m.end():]): return m.group(0)
        v = int(m.group(1), 16)
        a, b, g, r = v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255
        if v == 0: return 'THEME_NONE'
        if v == 0xFFFFFFFF and (IMAGE.search(code) or not re.search(r'col|Col|COL|u32|U32|Text|text|icon|Icon|draw|Draw|Rect|Line|Circle|=|\(', code)):
            return 'THEME_IMAGE_TINT'
        if v == 0xFFFFFFFF and re.search(r'(==|!=)\s*$', pre): return m.group(0)
        tok = classify(r, g, b, text_ctx)
        if at_file_scope: report.append('%s:%d table %s' % (fn, ln, tok)); return '%s /*a=%d*/' % (tok, a) if a != 255 else tok
        return tok_expr(tok, a)
    out = HEX.sub(hex_sub, out)
    def col_sub(m):
        rgb = [m.group(i) for i in (1, 2, 3)]
        if not all(NUM.match(x) for x in rgb): return m.group(0)
        r, g, b = (int(x) for x in rgb)
        a = m.group(4)
        a = 255 if a is None else int(a) if NUM.match(a.strip()) else a.strip()
        if a == 0 and (r, g, b) == (0, 0, 0): return 'THEME_NONE'
        tok = classify(r, g, b, text_ctx)
        if at_file_scope: report.append('%s:%d table(col) %s' % (fn, ln, tok)); return tok
        return tok_expr(tok, a)
    out = COL.sub(col_sub, out)
    def v4_sub(m):
        r, g, b, a = (float(m.group(i)) for i in (1, 2, 3, 4))
        if (r, g, b, a) == (1, 1, 1, 1) and IMAGE.search(code): return m.group(0)
        if a == 0 and (r, g, b) == (0, 0, 0): return 'THVA(TK_N0, 0)'
        tok = classify(int(r * 255 + .5), int(g * 255 + .5), int(b * 255 + .5), text_ctx)
        if code.lstrip().startswith('#define'): report.append('%s:%d define %s' % (fn, ln, tok))
        return tok_expr(tok, a, True)
    out = V4.sub(v4_sub, out)
    return out + sep + comment

def scope_tracker():
    depth = 0
    def feed(line):
        nonlocal depth
        s = re.sub(r'"(\\.|[^"\\])*"', '""', line.split('//')[0])
        s = re.sub(r"'(\\.|[^'\\])*'", "''", s)
        before = depth
        depth += s.count('{') - s.count('}')
        return before
    return feed

def main():
    args = sys.argv[1:]
    apply = '--apply' in args
    files = [a for a in args if not a.startswith('--')]
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src')
    if not files:
        files = [os.path.join(root, f) for f in sorted(os.listdir(root)) if f.endswith('.c') and f not in ('theme.c', 'brand.c')]
    report = []
    changed = 0
    for path in files:
        src = open(path, encoding='utf-8').read()
        if 'cimgui' not in src and 'igGet' not in src and 'render_preview' not in path:
            continue
        lines = src.split('\n')
        feed = scope_tracker()
        out = []
        n = 0
        static_table = False
        for i, line in enumerate(lines, 1):
            depth = feed(line)
            st = re.match(r'\s*(static\s+)?(const\s+)?(uint32_t|ImU32|unsigned)\b.*\[.*\]\s*=', line) is not None
            if st: static_table = line.strip().startswith('static') or depth == 0
            file_scope = (depth == 0 or static_table)
            nl = convert_line(line, report, os.path.basename(path), i, file_scope and (st or static_table))
            if static_table and ';' in line.split('//')[0]: static_table = False
            if nl != line: n += 1
            out.append(nl)
        if n:
            changed += n
            print('%-28s %4d lines' % (os.path.basename(path), n))
            if apply:
                txt = '\n'.join(out)
                if '#include "theme.h"' not in txt:
                    m = re.search(r'^#include "[^"]+"\s*$', txt, re.M)
                    pos = m.end() if m else 0
                    txt = txt[:pos] + '\n#include "theme.h"' + txt[pos:]
                open(path, 'w', encoding='utf-8', newline='').write(txt)
    print('%d lines changed' % changed)
    for r in report: print(r)

if __name__ == '__main__':
    main()
