// (Q16) The test of everything editable in the game (ingame_test.c, notas-dev/em-jogo.md)
#pragma once
#include <stdbool.h>

struct Editor;
// props / cvars checked and timed; the Facility level with BP_InGameTest (mode: "loading" or "progressive") packaged
// into package_dir (NULL: only the level in the editor). Prints "In-game settings: PASS"
bool ingame_test_setup(struct Editor* ed, const char* mode, const char* package_dir);
