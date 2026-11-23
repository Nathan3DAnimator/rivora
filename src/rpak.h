// The packaged game's single content file (B15a; notas-dev/jogo.md "Pacote único"): Content.rpak next to the game's
// executable holds every file of its Content folder, each deflated (or stored when that doesn't pay), found by a sorted
// index; an FNV-1a 64 of the index and of every file's bytes checks it at mount. Mounted, the platform's file calls
// (re_fopen to read, re_map_file, re_file_stat, re_dir_walk) answer for the paths under its root from the pack, as if
// the files were there; the rest go to the disk as before.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct RpakStats { uint32_t files; uint64_t raw, packed; double ms; } RpakStats;

// Every file under dir (recursively) into out (paths relative to dir, '/'); false: unread / unwritten
bool rpak_write_dir(const char* dir, const char* out, RpakStats* st);
// The pack answers for the paths under root ("<exe>/Content"); false: not a pack, or it fails its check
bool rpak_mount(const char* pack, const char* root);
bool rpak_mounted(void);

// (the platform's file calls: platform_files.c, platform_win32.c) a path under the mounted root and in the pack
bool rpak_has(const char* path, uint64_t* size, bool* is_dir);
uint8_t* rpak_read(const char* path, uint64_t* size); // malloc'd, the caller frees; NULL: not in the pack
// its files and folders right under dir (fn as re_dir_walk's: a name, is_dir, size); false: dir not in the pack
struct ReDirEntry;
bool rpak_walk(const char* dir, void (*fn)(void* user, const struct ReDirEntry* e), void* user);
