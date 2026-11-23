// (Q18) UDIM textures (notas-dev/udim.md): a texture made of tiles, one per unit square of the UV, numbered as Mari,
// Blender and the Unreal do: 1001 is u 0..1, v 0..1; 1002 the next one to the right (u 1..2); ten per row, 1011 the
// row above (v 1..2 in Blender's V up; in the engine's V down, v -1..0).
//
// The asset: a .rudim file (binary, little-endian, mapped as it is), each tile's mips already compressed (BC7 for colors
// and masks, BC5 for normal maps: R and G kept, B rebuilt from them) and its mean color. Made by udim_import from a
// folder of tiles or from one tile's file (<base>.1001.png .. <base>.10xx.png; <base>_1001.png; <base>_u1_v1.png).
// A material's Image Texture node reads it like any image (the tile from the UV: material_udim.inc), or a pattern with
// Blender's token ("<base>.<UDIM>.png": the Tlender's and Blender's way) read from the tiles' files as they are.
//
// Layout (version 1):
//   RUdimHeader (header_size bytes; later versions only grow it, readers skip what they don't know)
//   RUdimTile x tile_count (tile_entry_size bytes each, same rule), from table_offset
//   each tile's mips, largest first, from its data_offset (64-byte aligned): BC blocks row by row ((w+3)/4 x (h+3)/4
//   blocks of 16 bytes), or RGBA8 rows
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UDIM_MAGIC 0x4D445552u // "RUDM"
#define UDIM_VERSION 1u
#define UDIM_FIRST 1001
#define UDIM_COLS 10   // tiles a row (u 0..10)
#define UDIM_ROWS 10   // rows read (1001 .. 1100: the Unreal's and most tools' range)
#define UDIM_MAX_TILES (UDIM_COLS * UDIM_ROWS)
#define UDIM_MAX_SIZE 8192u

enum { UDIM_FMT_RGBA8 = 0, UDIM_FMT_BC7 = 1, UDIM_FMT_BC5 = 2 };
enum {
    UDIM_F_NORMAL = 1u, // a normal map (BC5: B = sqrt(1 - R^2 - G^2), the shaders rebuild it)
    UDIM_F_SRGB = 2u,   // color data (its mips averaged in linear light)
};

typedef struct RUdimHeader {
    uint32_t magic;
    uint16_t version, header_size;
    uint32_t flags;           // UDIM_F_
    uint32_t format;          // UDIM_FMT_
    uint32_t tile_count;
    uint32_t tile_entry_size;
    uint64_t table_offset;
    uint64_t file_size;
    char source[24];          // the source pattern's name, for the eye (cut)
} RUdimHeader;
_Static_assert(sizeof(RUdimHeader) == 64, "RUdimHeader");

typedef struct RUdimTile {
    uint32_t udim;            // 1001 ..
    uint32_t width, height, mips;
    uint64_t data_offset, data_size;
    uint32_t avg_rgba;        // the tile's mean (RGBA8, as stored: sRGB for colors)
    uint32_t reserved[3];
} RUdimTile;
_Static_assert(sizeof(RUdimTile) == 48, "RUdimTile");

// A tile number's place: column 0..9, row 0..9 (false: out of 1001..1100)
static inline bool udim_tile_xy(int udim, int* x, int* y) {
    int k = udim - UDIM_FIRST;
    if (k < 0 || k >= UDIM_MAX_TILES) return false;
    *x = k % UDIM_COLS, *y = k / UDIM_COLS;
    return true;
}
// The tile under an engine UV (V down: the row above 1001 is v -1..0) and the tile's origin there
static inline int udim_of_uv(float u, float v) {
    int x = (int)(u >= 0.0f ? u : u - 1.0f), fy = (int)(v >= 0.0f ? v : v - 1.0f); // floor
    int y = -fy;
    if (x < 0 || x >= UDIM_COLS || y < 0 || y >= UDIM_ROWS) return 0;
    return UDIM_FIRST + x + y * UDIM_COLS;
}
static inline void udim_origin(int udim, float* u0, float* v0) {
    int x = 0, y = 0;
    udim_tile_xy(udim, &x, &y);
    *u0 = (float)x, *v0 = (float)-y;
}

// --- Names ---
// A path naming a UDIM texture: a .rudim asset, or a pattern with Blender's <UDIM> (or Mudbox's <UVTILE>) token
bool udim_is_path(const char* path);
// One tile's file name split around its number (<base>.1001.png, <base>_1001.tif, <base>_u1_v1.png): the pattern with
// the token in its place ("<base>.<UDIM>.png"), the tile. False: not a tile's name
bool udim_pattern_of(const char* path, char* pattern, size_t cap, int* udim);
// The pattern's file for a tile ("<UDIM>" -> 1001, "<UVTILE>" -> u1_v1)
void udim_tile_path(const char* pattern, int udim, char* out, size_t cap);
// The tiles a pattern has on disk (sorted); their count
int udim_find_tiles(const char* pattern, int* udims, int cap);
// A file an importer brings: a tile of a UDIM set with at least one other tile beside it (a lone "x.1001.png" stays a
// plain image). pattern: the set's (may be NULL)
bool udim_detect(const char* path, char* pattern, size_t cap);
// A normal map by its name (normal, nrm, nor, _n.): BC5
bool udim_name_is_normal(const char* path);

// --- Import: a folder, a pattern or one tile's file -> .rudim ---
typedef struct UdimImportOpts {
    int normal;          // -1: by the name, 0: color or mask (BC7), 1: normal map (BC5)
    uint32_t max_size;   // tiles bigger are halved down to it (0: UDIM_MAX_SIZE)
    bool rgba8;          // no compression (the comparison's)
} UdimImportOpts;
typedef struct UdimImportReport {
    int tiles;
    uint32_t w, h;       // the largest tile
    uint64_t bytes;      // the file's
    double ms, encode_ms;
    double psnr_min;     // the worst tile's mip 0 against its source (dB; 0: not measured)
    char pattern[512];
    char err[200];
} UdimImportReport;
bool udim_import(const char* src, const char* out_path, const UdimImportOpts* o, UdimImportReport* r);
// The .rudim an import of src makes in folder: "<folder>/<base>.rudim" (base: the pattern's name before the token)
void udim_asset_name(const char* src_pattern, const char* folder, char* out, size_t cap);

// --- Reading (mapped) ---
typedef struct UdimFile {
    struct ReFileMap* map;
    const uint8_t* data;
    uint64_t size;
    uint32_t version, flags, format, count;
    RUdimTile tiles[UDIM_MAX_TILES]; // normalized from the file's entries (whatever their size)
} UdimFile;
bool udim_open(const char* path, UdimFile* f, char* err, size_t err_cap);
void udim_close(UdimFile* f);
int udim_find(const UdimFile* f, int udim); // its index in tiles (-1: none)
// A tile's mip: its data, size in texels and bytes (NULL: none)
const uint8_t* udim_mip(const UdimFile* f, int tile, uint32_t mip, uint32_t* w, uint32_t* h, size_t* bytes);
size_t udim_mip_bytes(uint32_t format, uint32_t w, uint32_t h);
// A tile's mips from one on asked from the disk ahead (the mapped pages read in the background: Windows'
// PrefetchVirtualMemory), so the upload a few frames later doesn't wait for the disk
void udim_prefetch(const UdimFile* f, int tile, uint32_t from_mip);

// --- The codecs (CPU; the import's and the tests') ---
void udim_bc7_encode_block(const uint8_t rgba[64], uint8_t out[16]); // mode 6 (bc7.comp's, on the CPU)
void udim_bc7_decode_block(const uint8_t in[16], uint8_t rgba[64]);  // modes 6 (others: magenta)
void udim_bc5_encode_block(const uint8_t rgba[64], uint8_t out[16]); // R and G
void udim_bc5_decode_block(const uint8_t in[16], uint8_t rgba[64]);  // B rebuilt from R and G, A 255

int udim_selftest(void);
// A test tile as a PNG: a smooth function of the whole UDIM space (continuous across the tiles' edges: a seam shows), a
// normal map's when normal
void udim_write_test_tile(const char* path, int udim, uint32_t w, uint32_t h, bool normal); // --udim-test: names, import, codecs, versions; failures (0: all passed)
