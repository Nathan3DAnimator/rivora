// File system helpers (copied from the Tlender, src/platform_files.h): folder walks with sizes and dates, nested
// folders, moves, the recycle bin and read-only memory maps (the .rasset loader maps files). UTF-8 paths.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct ReDirEntry {
    const char* name; // UTF-8, no folder
    bool is_dir;
    uint64_t size;
    int64_t mtime;    // seconds since 1970 (UTC)
} ReDirEntry;

// Calls fn for every file and folder of dir (one level; "." and ".." left out). False when it can't be read.
bool re_dir_walk(const char* dir, void (*fn)(void* user, const ReDirEntry* e), void* user);
// Size, modification time and kind of a path. False when it doesn't exist.
bool re_file_stat(const char* path, uint64_t* size, int64_t* mtime, bool* is_dir);
bool re_make_dirs(const char* path);              // every missing folder of the path; true if it exists afterwards
bool re_move_path(const char* src, const char* dst); // a file or a folder; fails when dst exists
bool re_trash_path(const char* path);             // to the Recycle Bin (Windows; elsewhere removed)
// Removes a folder and everything inside it for good (tests and caches only).
bool re_remove_tree(const char* path);
int64_t re_unix_time(void);                       // now, seconds since 1970 (UTC)
// fopen with Windows' share modes: deny_write keeps other processes from writing the file while it is open (a
// library pack has one writer). Elsewhere a plain fopen.
FILE* re_fopen_share(const char* path, const char* mode, bool deny_write);

// Read-only view of a whole file (NULL when it can't be opened or is empty). The file can still be written and grown
// through another handle; the view keeps the size it was made with.
typedef struct ReFileMap ReFileMap;
ReFileMap* re_map_file(const char* path, const uint8_t** data, uint64_t* size);
void re_unmap_file(ReFileMap* m);

// Copies a file (fails when dst exists). UTF-8 paths.
bool re_copy_file(const char* src, const char* dst);
// The executable's folder (UTF-8, no separator at the end).
bool re_exe_dir(char* out, size_t cap);
// Opens Explorer on a folder, or on a file's folder with the file selected.
void re_show_in_explorer(const char* path);
// Runs a command line to its end, no window, its output (stdout and stderr) into log_path ("": dropped); its exit code
// (-1: it could not start). Blocks: run it from a worker (async.h)
int re_run_command(const char* cmdline, const char* log_path);
