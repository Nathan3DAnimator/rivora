// Deflate and inflate (copied from the Tlender's io_fbx_zlib.c): the updater's zip packages and the report zips.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint32_t re_adler32(const uint8_t* p, size_t n);
// A zlib stream into exactly out_len bytes. False on any error or size mismatch.
bool re_inflate(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len);
bool re_inflate_raw(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len); // raw deflate (zip entries)
// A zlib stream of the data (malloc'd), NULL without memory.
uint8_t* re_deflate(const uint8_t* in, size_t n, size_t* out_len);
void re_deflate_thread_free(void); // frees the calling thread's compression tables (threads that end)
