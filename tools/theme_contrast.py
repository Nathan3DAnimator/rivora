# The themes' contrast (Q1, notas-dev/temas.md): every built-in theme and every .rtheme given (or the user's folder,
# %APPDATA%\RealEngine\Themes, with --user) checked by WCAG 2: the texts 4.5:1 on each surface they sit on, the accent,
# states, icons and the node headers' text 3:1. The checks are the engine's own (theme_check_contrast, the Theme
# Editor shows the same warnings): this calls re_test_theme --contrast.
#   python tools/theme_contrast.py <re_test_theme.exe> [--user] [files.rtheme...]
import glob, os, subprocess, sys

def main():
    if len(sys.argv) < 2:
        print(__doc__ if __doc__ else 'usage: theme_contrast.py <re_test_theme.exe> [--user] [files.rtheme...]')
        return 2
    exe = sys.argv[1]
    files = [a for a in sys.argv[2:] if a != '--user']
    if '--user' in sys.argv:
        files += glob.glob(os.path.join(os.environ.get('APPDATA', ''), 'RealEngine', 'Themes', '*.rtheme'))
    r = subprocess.run([exe, '--contrast'] + files, capture_output=True, text=True, encoding='utf-8', errors='replace')
    sys.stdout.write(r.stdout)
    if r.returncode != 0:
        print('FAIL: contrast below the minimum in the pairs above')
    return r.returncode

if __name__ == '__main__':
    sys.exit(main())
