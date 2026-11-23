# The theme_literals test (ctest, area editor; notas-dev/temas.md): fails when a file of src/ has a color literal
# outside theme.c. A color is a token of the theme (TH(TK_...), THA, THV, THVA); the checker is the conversion's own
# (tools/theme_convert.py: a line it would change has a literal). Exceptions: theme.c (the themes), brand.c (the
# brand's colors) and a line marked "theme: literal (<why>)" (a color that is data, not the UI's).
#   python tools/theme_literals.py [src_dir]
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import theme_convert as tc

def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src')
    bad = []
    files = 0
    for f in sorted(os.listdir(root)):
        if not f.endswith('.c') or f in ('theme.c', 'brand.c'):
            continue
        src = open(os.path.join(root, f), encoding='utf-8', errors='replace').read()
        if 'cimgui' not in src and 'igGet' not in src and 'theme.h' not in src:
            continue
        files += 1
        feed = tc.scope_tracker()
        for i, line in enumerate(src.split('\n'), 1):
            feed(line)
            if tc.convert_line(line, [], f, i, False) != line:
                bad.append('%s:%d: %s' % (f, i, line.strip()[:160]))
    if bad:
        print('FAIL: %d color literal(s) outside theme.c (use a theme token: TH(TK_...), THA, THV, THVA; see notas-dev/temas.md):' % len(bad))
        for b in bad: print('  ' + b)
        return 1
    print('theme_literals: OK, no color literal in %d UI files of src/ (the themes are in theme.c)' % files)
    return 0

if __name__ == '__main__':
    sys.exit(main())
