# The tips_audit test (ctest, area editor; notas-dev/icones-e-tooltips.md): fails when a control of the editor has no
# description. Every widget call of src/ with a literal label (igButton, igMenuItem, ui_menu_item, igCheckbox, the
# fields, tabs, headers, combos, ui_icon_button, the Details rows' labels...) must find its label (the English msgid,
# its visible part without the ## id; or the ## id of an icon's button) in ui_tips_table.c, the same lookup the editor
# does (ui_tip_find). A "##id" field within a few lines after a Details row's label (label_cell, row_*, PL,
# ui_tip_last_item) takes the row's description, as the editor does (ui_tip_row). Skipped: a label that is data (a
# format with %), the game's own widgets (ui_widget_draw.c, hud_widget.c: the player's UI, not the editor's) and a
# line marked "tips: none (<why>)".
#   python tools/tips_audit.py [src_dir] [--list]
import os, re, sys

WIDGETS = (
    r'igButton|igSmallButton|igCheckbox|igCheckboxFlags_IntPtr|igRadioButton_Bool|igRadioButton_IntPtr|igSelectable_Bool|'
    r'igSelectable_BoolPtr|igMenuItem_Bool|igMenuItem_BoolPtr|igBeginMenu|igBeginTabItem|igCollapsingHeader_TreeNodeFlags|'
    r'igCollapsingHeader_BoolPtr|igTreeNodeEx_Str|igTreeNode_Str|igBeginCombo|igCombo_Str_arr|igCombo_Str|igCombo_FnStrPtr|'
    r'igSliderFloat|igSliderFloat2|igSliderFloat3|igSliderInt|igSliderInt2|igSliderAngle|igDragFloat|igDragFloat2|'
    r'igDragFloat3|igDragFloat4|igDragFloatRange2|igDragInt|igDragInt2|igDragInt3|igDragIntRange2|igInputFloat|igInputFloat2|'
    r'igInputFloat3|igInputInt|igInputInt2|igInputText|igInputTextWithHint|igInputTextMultiline|igColorEdit3|igColorEdit4|'
    r'igColorButton|igDragScalar|igSliderScalar|igInputScalar|igInputDouble|igArrowButton|igInvisibleButton')
ICON_WIDGETS = r'ui_menu_item|ui_menu_item_ptr|ui_begin_menu'  # (icon, label, ...)
LIT = r'(?:TR\(\s*|PL\(\s*TR\(\s*|PL\(\s*)?"((?:[^"\\]|\\.)*)"'
CALL = re.compile(r'\b(' + WIDGETS + r')\(\s*' + LIT)
ICALL = re.compile(r'\b(' + ICON_WIDGETS + r')\(\s*[A-Z_0-9]+\s*,\s*' + LIT)
BCALL = re.compile(r'\bui_icon_button\(\s*"((?:[^"\\]|\\.)*)"\s*,\s*[A-Z_0-9]+\s*,\s*(?:TR\(\s*)?(NULL|"(?:[^"\\]|\\.)*")')
ROWCALL = re.compile(r'\b(label_cell|row_[a-z0-9_]+|ui_tip_last_item)\(\s*(?:TR\(\s*)?"((?:[^"\\]|\\.)*)"')
ROWANY = re.compile(r'\b(label_cell|row_[a-z0-9_]+|ui_tip_last_item|ui_prop_label|PL)\(')
TIPLINE = re.compile(r'^\s*T\(\s*"((?:[^"\\]|\\.)*)"')
GAME_UI = ('ui_widget_draw.c', 'hud_widget.c', 'inventory_widget.c', 'quest_hud.c', 'ability_widget.c')

def unescape(s):
    return s.replace('\\"', '"').replace('\\\\', '\\')

def tip_labels(root):
    labels = set()
    for f in ('ui_tips_table.c', 'ui_tips_table_q5.c', 'ui_tips_table_q8.c', 'ui_tips_table_q11.c', 'ui_tips_table_q12.c', 'ui_tips_table_q13.c', 'ui_tips_table_q14.c', 'ui_tips_table_q15.c', 'ui_tips_table_q16.c', 'ui_tips_table_q17.c', 'ui_tips_table_q18.c', 'ui_tips_table_q19.c', 'ui_tips_table_q20.c', 'ui_tips_table_q21.c', 'ui_tips_table_q25.c', 'ui_tips_table_q23.c', 'ui_tips_table_look.c'):
        if not os.path.exists(os.path.join(root, f)): continue
        for line in open(os.path.join(root, f), encoding='utf-8', errors='replace'):
            m = TIPLINE.match(line)
            if m: labels.add(unescape(m.group(1)))
    return labels

def key_of(label):
    """What ui_tip_find looks up: the visible part (no ## id, trimmed); else the id. None: data, skipped."""
    vis, _, rest = label.partition('##')
    vis = vis.strip()
    ident = rest.lstrip('#')
    if '%' in vis or '%' in ident:
        return None, None
    if vis and not re.search(r'[A-Za-z]{2}', vis):
        vis = ''  # (a symbol, a number: its id names it)
    return (vis or None), (ident or None)

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    root = args[0] if args else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src')
    tips = tip_labels(root)
    missing, total, by_row = {}, 0, 0
    for f in sorted(os.listdir(root)):
        if not f.endswith('.c') or f.startswith('ui_tips_table') or f in GAME_UI:
            continue
        lines = open(os.path.join(root, f), encoding='utf-8', errors='replace').read().split('\n')
        for i, line in enumerate(lines, 1):
            if 'tips: none' in line:
                continue
            found = [unescape(m.group(2)) for m in CALL.finditer(line)]
            found += [unescape(m.group(2)) for m in ICALL.finditer(line)]
            for m in BCALL.finditer(line):  # an icon's button: its label (drawn), else its id; its own tip text is its tooltip
                lab = m.group(2)
                if re.search(r'TR\(\s*"', line[m.end():]):
                    total += 1
                    continue
                found.append(unescape(lab[1:-1]) + '##' + m.group(1) if lab != 'NULL' else '##' + m.group(1))
            found += [unescape(m.group(2)) for m in ROWCALL.finditer(line)]
            if not found:
                continue
            row_near = any(ROWANY.search(lines[k]) for k in range(max(0, i - 5), i))
            for lab in found:
                vis, ident = key_of(lab)
                if not vis and not ident:
                    continue
                total += 1
                if (vis and vis in tips) or (ident and (ident in tips or '##' + ident in tips)):
                    continue
                if not vis and row_near:  # (a field of a Details row: the row's description)
                    by_row += 1
                    continue
                missing.setdefault(vis or ('##' + ident), []).append('%s:%d' % (f, i))
    if missing:
        print('FAIL: %d control label(s) of %d without a description in ui_tips_table.c:' % (len(missing), total))
        for k in sorted(missing):
            print('  "%s"  (%s)' % (k, ', '.join(missing[k][:3]) + (' ...' if len(missing[k]) > 3 else '')))
        return 1
    print('tips_audit: OK, %d control labels, every one with its description (%d by their Details row; %d descriptions)'
          % (total, by_row, len(tips)))
    return 0

if __name__ == '__main__':
    sys.exit(main())
