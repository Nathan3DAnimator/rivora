#!/usr/bin/env python3
"""(Q16, notas-dev/em-jogo.md) What is still "editor only": the sweep (and the test ingame_sweep).

It lists:
  1. the Details controls with no path in the game: every control of the Details panels (tools/gen_props.py's parse)
     that Set / Get Property by Name doesn't reach (a value copied by hand, a label that isn't text, an array by index,
     an asset reference...), and the editor's own settings (ed->gi, ed->msaa) without a console variable;
  2. the Project Settings fields with no console variable, apart from the project's definitions (its name, maps,
     classes, packaging, platforms, network: not settings a running game changes);
  3. the systems the editor's frame runs and the game's loop doesn't (the editor's own tools apart).

    python tools/ingame_sweep.py                    # the report
    python tools/ingame_sweep.py --check            # the test: props_table.inc fresh, nothing new "editor only"
    python tools/ingame_sweep.py --markdown out.md  # the list for the notes
    python tools/ingame_sweep.py --write-baseline   # accept today's list (tools/ingame_sweep_baseline.txt)
A new Details control, a new Project Settings field or a new system of the editor's frame that the game can't reach
fails --check until it gets a path (gen_props.py picks it up by itself when it is a plain row of a component's field;
else a console variable, a node) or is added to the baseline on purpose.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_props  # noqa: E402

ROOT, SRC = gen_props.ROOT, gen_props.SRC
BASELINE = os.path.join(ROOT, 'tools', 'ingame_sweep_baseline.txt')

# Project Settings fields that define the project rather than set a running game (never a console variable)
PROJECT_DEFINITIONS = re.compile(
    r'^(name|company|version|description|copyright|game_name|icon|splash|show_intro|editor_startup_map|game_default_map|'
    r'main_menu_map|game_mode|default_pawn|player_controller|hud|game_state|player_state|game_instance|default_imc|'
    r'key_\w+|package_\w+|include_all_assets|shipping|android_\w+|online_steam|steam_\w+|net_\w+|emu_\w+|'
    r'window_title|resolution_[wh]|fullscreen|vsync|mouse_capture|hdr_\w+|render_mode|ray_tracing|static_lighting|'
    r'game_scalability|pause_menu|pause_menu_widget|pie_pause_key|save_game_class|invert_mouse_y)$')
# (window, VSync, resolution: the player's options carry them in the game, Set Game User Setting / r.VSync; ray tracing:
# a device's feature chosen at start; the game's scalability: sg.Scalability)

# Details controls the game reaches by a node of their own (not by name): label -> the node
BY_NODE = {
    'Tick When Paused': 'Set Tickable When Paused (its instance too)',
    'Actor Outline Color': 'Set Actor Outline (Q11)',
    'Flicker Amount': 'Set Light Flicker (Q13)',
    'Intensity': "the light's Intensity in candela/lux by name (this row shows it in the chosen unit)",
    'Hidden in Editor': 'editor only by nature (Set Actor Hidden In Game in the game)',
    'Traffic Road': "the road's Lanes Each Way by name (0: not a road; Q27.2)",
    'Smart Object Slots': 'Set Smart Object (Q27.10)',
    'Opens At': 'Set Smart Object (Q27.10)',
    'Closes At': 'Set Smart Object (Q27.10)',
}

# the editor frame's calls the game doesn't make: why (a tool of the editor, or where the game does it)
EDITOR_SYSTEMS = {
    'bake_tick': 'covered: the editor saves its Build Lighting; the game has rtl_frame (Q16)',
    'game_bake_tick': 'covered: rtl_frame in the editor\'s Play (Q16)',
    'bp_components_update': 'covered: the game runs it inside bp_tick',
    'editor_construct_update': 'editor tool: the Construction Scripts while editing (the game runs them at spawn)',
    'take_record_tick': 'editor tool: the Take Recorder records the editor\'s Play into a Level Sequence',
    'levels_frame': 'editor tool: the Levels panel (the game streams by Load Stream Level / World Partition)',
    'project_ui_frame': 'editor tool: Project Settings / Package Game windows',
    'services_frame': 'editor tool: the update checker, the reports',
    'editor_cost_frame': 'editor tool: the per-actor cost overlay (the game has the Debug Viewer)',
    'async_update': 'editor tool: the background tasks\' cards',
    'vehicle_editor_update': 'editor tool: the Vehicle editor\'s preview',
    'editor_frame': 'editor tool: the frame itself',
    'test_step': 'editor tool: the tests\' scripted steps',
    'dv_frame': 'covered: the game has the Debug Viewer (F10)',
    'panic_tick': "editor tool: the --scene panicN test's scripted alarm (the game's alarms: the Crowd Alarm node; Q27.1)",
}


def frame_calls(path, start_pat, end_pat=r'^\}'):
    t = gen_props.strip_comments(gen_props.read(path))
    m = re.search(start_pat, t, flags=re.M)
    if not m:
        return set()
    e = re.search(end_pat, t[m.end():], flags=re.M)
    body = t[m.end():m.end() + (e.start() if e else len(t))]
    return set(re.findall(r'\b([a-z_0-9]+_(?:update|tick|frame|step|sync|apply))\(', body))


def cvar_fields():
    t = gen_props.read(os.path.join(SRC, 'cvars.c'))
    gi = set(re.findall(r'GI_[FIB]\("[^"]+",\s*(\w+)', t))
    pr = set(re.findall(r'PR_[FIBV]\("[^"]+",\s*(\w+)', t))
    gus = set(re.findall(r'GUS\("[^"]+",\s*"(\w+)"', t))
    if '"msaa"' in t:
        gi.add('msaa')
    return gi, pr, gus


def sweep():
    items = []  # (key, category, text)
    rows = gen_props.details_controls()
    gi_f, pr_f, _ = cvar_fields()
    reached = 0
    editor_own, by_node = [], []
    for r in rows:
        if not r['why']:
            reached += 1
            continue
        lab = r['label'] or '?'
        em = re.fullmatch(r'&?\s*ed\s*->\s*(gi\.)?(\w+)', r['ptr'].strip())
        if em and (em.group(2) in gi_f):
            reached += 1
            continue
        if re.match(r'&?\s*(ed\s*->|g_pie_)', r['ptr'].strip()):  # the editor's own view, brushes, Play options
            editor_own.append('%s %s() "%s" (%s)' % (r['file'], r['func'], lab, r['ptr'][:40]))
            continue
        if lab in BY_NODE:
            by_node.append('%s "%s": %s' % (r['func'], lab, BY_NODE[lab]))
            continue
        key = 'details:%s:%s:%s' % (r['file'], r['func'], lab)
        items.append((key, 'details', '%s %s() "%s" (%s): %s' % (r['file'], r['func'], lab, r['ptr'][:48], r['why'])))
    # the Project Settings
    t = gen_props.read(os.path.join(SRC, 'project_ui.c'))
    fields = sorted(set(re.findall(r'\bp->(\w+)', t)))
    ps = gen_props.parse_structs()[0].get('ProjectSettings', {})
    defs, proj_reached = 0, 0
    for f in fields:
        if f not in ps:
            continue  # (a local struct's p->...)
        if PROJECT_DEFINITIONS.match(f):
            defs += 1
            continue
        if f in pr_f:
            proj_reached += 1
            continue
        items.append(('project:%s' % f, 'project', 'Project Settings field %s: no console variable' % f))
    # the systems
    ed = frame_calls(os.path.join(SRC, 'editor.c'), r'^void editor_frame\(')
    game = frame_calls(os.path.join(SRC, 'game_main.c'), r'while \(!glfwWindowShouldClose\(G\.window\)\)', r'^    \}$')
    sys_tools, sys_cov = [], []
    for c in sorted(ed - game):
        why = EDITOR_SYSTEMS.get(c)
        if why and why.startswith('covered'):
            sys_cov.append((c, why))
        elif why:
            sys_tools.append((c, why))
        else:
            items.append(('system:%s' % c, 'system', 'the editor\'s frame runs %s(); the game\'s loop doesn\'t' % c))
    stats = dict(by_node=by_node, editor_own=editor_own, details=len(rows), reached=reached, defs=defs, proj_reached=proj_reached, sys_tools=sys_tools, sys_cov=sys_cov)
    return items, stats


def main():
    items, st = sweep()
    stale = gen_props.render(gen_props.details_controls()) != (gen_props.read(gen_props.OUT) if os.path.exists(gen_props.OUT) else '')
    base = set()
    if os.path.exists(BASELINE):
        base = {l.strip() for l in open(BASELINE, encoding='utf-8') if l.strip() and not l.startswith('#')}
    keys = {k for k, _, _ in items}
    new = [i for i in items if i[0] not in base]
    gone = sorted(base - keys)
    if '--write-baseline' in sys.argv:
        with open(BASELINE, 'w', encoding='utf-8', newline='\n') as f:
            f.write('# tools/ingame_sweep.py --write-baseline: what is "editor only" today, on purpose (notas-dev/em-jogo.md)\n')
            for k in sorted(keys):
                f.write(k + '\n')
        print('ingame_sweep: baseline written (%d items)' % len(keys))
        return 0
    if '--markdown' in sys.argv:
        out = sys.argv[sys.argv.index('--markdown') + 1]
        with open(out, 'w', encoding='utf-8', newline='\n') as f:
            f.write('# Ainda "só editor" (tools/ingame_sweep.py)\n\n')
            f.write('- Details: %d controles, %d alcançáveis em jogo (Set/Get Property by Name ou console variable)\n' % (st['details'], st['reached']))
            f.write('- Project Settings: %d campos com console variable; %d são definições do projeto (não ajustes de jogo)\n' % (st['proj_reached'], st['defs']))
            for cat, title in (('details', 'Controles do Details sem caminho em jogo'), ('project', 'Project Settings sem console variable'),
                               ('system', 'Sistemas que só o editor roda')):
                rows = [x for k, c, x in items if c == cat]
                f.write('\n## %s (%d)\n' % (title, len(rows)))
                for x in rows:
                    f.write('- %s\n' % x)
            f.write('\n## Alcançados por um nó próprio (%d)\n' % len(st['by_node']))
            for x in st['by_node']:
                f.write('- %s\n' % x)
            f.write('\n## Ajustes do próprio editor: vista, pincéis, Play (%d)\n' % len(st['editor_own']))
            for x in st['editor_own']:
                f.write('- %s\n' % x)
            f.write('\n## Sistemas só do editor por natureza (%d)\n' % len(st['sys_tools']))
            for c, why in st['sys_tools']:
                f.write('- `%s`: %s\n' % (c, why))
        print('ingame_sweep: %s written' % out)
    print('ingame_sweep: Details %d controls, %d reach the game; Project Settings: %d by console variable, %d definitions; '
          '%d editor-only items (%d Details, %d Project Settings, %d systems); %d editor tools' %
          (st['details'], st['reached'], st['proj_reached'], st['defs'], len(items), sum(1 for i in items if i[1] == 'details'),
           sum(1 for i in items if i[1] == 'project'), sum(1 for i in items if i[1] == 'system'), len(st['sys_tools'])))
    if '--check' not in sys.argv:
        for _, _, x in items:
            print('  ' + x)
        return 0
    bad = False
    if stale:
        print('ingame_sweep: FAIL: src/props_table.inc is stale (run python tools/gen_props.py)')
        bad = True
    for _, _, x in new:
        print('ingame_sweep: FAIL: new editor-only item: %s' % x)
        bad = True
    for k in gone:
        print('ingame_sweep: now reached in the game (drop it from the baseline): %s' % k)
    if not bad:
        print('ingame_sweep: OK')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
