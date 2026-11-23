// The FBX document (fbx_read.c, copied from the Tlender's io_fbx_read.c / io_fbx_internal.h and adapted): binary 7.x
// (32- and 64-bit records, zlib arrays) and ASCII files parsed into a tree of nodes with typed properties. The FBX
// importer (import_fbx.inc) reads it.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// --- Parsed document (io_fbx_read.c) -----------------------------------------------------------------------------------
// Property types are the binary file's: Y int16, C bool/char, I int32, F float, D double, L int64, S string, R raw
// bytes, and the arrays f d l i b. ASCII files give D/L for numbers, S for strings and 'd' arrays of decoded doubles.
typedef struct FbxProp {
    char type;
    uint8_t encoding;     // arrays of binary files: 0 raw, 1 zlib
    uint32_t count;       // array elements; string / raw byte length
    uint32_t comp_len;    // arrays: bytes in the file
    union {
        int64_t i;
        double d;
        const uint8_t* p; // arrays and raw bytes: into the file (or decoded doubles for ASCII); strings: NUL-ended copy
    };
} FbxProp;

typedef struct FbxNode {
    const char* name;
    FbxProp* props;
    uint32_t nprops;
    struct FbxNode* child; // first child
    struct FbxNode* next;  // next sibling
} FbxNode;

typedef struct FbxChunk FbxChunk;

typedef struct FbxDoc {
    uint8_t* data; // the file
    size_t size;
    bool ascii;
    int version;   // 7400...
    FbxNode root;  // the top-level nodes are its children
    FbxChunk* arena;
} FbxDoc;

// Reads and parses a file (binary or ASCII). False with the reason in err.
bool fbx_doc_load(FbxDoc* d, const char* path, char* err, size_t err_cap);
// Parses a buffer the document takes (freed with it).
bool fbx_doc_parse(FbxDoc* d, uint8_t* data, size_t size, char* err, size_t err_cap);
void fbx_doc_free(FbxDoc* d);

const FbxNode* fbx_find(const FbxNode* n, const char* name);    // first child with that name
const FbxNode* fbx_find_next(const FbxNode* n, const char* name); // next sibling after n with that name
static inline const FbxProp* fbx_prop(const FbxNode* n, uint32_t i) { return n && i < n->nprops ? &n->props[i] : NULL; }
int64_t fbx_prop_int(const FbxProp* p, int64_t def);
double fbx_prop_num(const FbxProp* p, double def);
const char* fbx_prop_str(const FbxProp* p); // "" when not a string
// The first property of a child node ("Version: 232" -> 232).
int64_t fbx_child_int(const FbxNode* n, const char* name, int64_t def);
double fbx_child_num(const FbxNode* n, const char* name, double def);
const char* fbx_child_str(const FbxNode* n, const char* name);
bool fbx_is_array(const FbxProp* p);
// The elements of an array property converted (malloc'd), NULL when it is not one or can't be read
double* fbx_array_f64(const FbxProp* p, size_t* count);
int32_t* fbx_array_i32(const FbxProp* p, size_t* count);
int64_t* fbx_array_i64(const FbxProp* p, size_t* count);
// An object's name and class from its name property ("name\0\1class" binary, "class::name" ASCII)
void fbx_name_class(const FbxProp* p, char* name, size_t cap, char* cls, size_t cls_cap);
