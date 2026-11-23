// Zip archives (the update packages: tools/make_dist.ps1 writes them with PowerShell's Compress-Archive): the central
// directory, stored and deflated entries, CRC-32 checked. No UI; also in the tests.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint32_t re_crc32(uint32_t crc, const void* data, size_t len); // start with 0

// Every file of the archive into out_dir (created). strip_top: when every entry is inside one folder (the package's
// Real Engine-<version>-win64/), that folder is left out. Entries that would land outside out_dir ("..", absolute paths)
// are refused. files gets the count written. False with the reason in err (whatever was written stays).
bool zip_extract(const char* zip_path, const char* out_dir, bool strip_top, int* files, char* err, size_t err_cap);

// A zip of files from memory (stored or deflated), for the tests' packages. names[i] with '/' for folders.
bool zip_write(const char* zip_path, const char* const* names, const uint8_t* const* data, const size_t* sizes, int count,
               bool deflate);
