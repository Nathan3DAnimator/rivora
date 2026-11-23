// (A9.5a: copied from the Tlender, ../tlender/src/vdb.h, adapted: the engine's zlib and files; the format unchanged)
// OpenVDB files (.vdb): the exchange format of EmberGen, LiquiGen, Houdini, Blender's smoke/liquid caches. Read and
// written without the OpenVDB library (own reader/writer of the file format), into / from VolumeGridSet.
//
// Contract of the physics wave: the smoke & fire session implements it (vdb.c, replacing vdb_stub.c); the liquids and
// granular sessions call it as is.
#pragma once
#include <stdbool.h>

#include "volume_grid.h"

typedef struct VdbInfo {
    int file_version;       // OpenVDB file format version (222, 224...)
    int grid_count;
    char names[16][VG_NAME_LEN];
    int types[16];          // VolumeGridType, -1 when the grid's type is not supported
    bool compressed;        // blosc / zip
} VdbInfo;

typedef struct VdbWriteOptions {
    bool half_float;        // 16-bit values (Blender's and EmberGen's default)
    bool compress;          // zip compression of the leaf buffers
} VdbWriteOptions;

bool vdb_probe(const char* path, VdbInfo* out, char* err, int cap);
// Reads every supported grid of the file (only the ones named in `names` when it is not NULL).
bool vdb_read(const char* path, VolumeGridSet* out, const char* const* names, int name_count, char* err, int cap);
bool vdb_write(const char* path, const VolumeGridSet* set, const VdbWriteOptions* opt, char* err, int cap);
// A sequence "fire_0001.vdb" .. : the path of a frame (the last run of digits replaced, as Blender's sequences).
void vdb_sequence_path(const char* first, int frame, char* out, int cap);
