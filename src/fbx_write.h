// A binary FBX 7.4 writer (fbx_write.c, 2026-11-20): the nodes and their typed properties in the file's own layout (32-bit
// records, raw arrays), the header, the null records and the footer the FBX SDK checks (Blender's io_scene_fbx writes
// the same). What fills it is the caller's: vr_hand.c (the VR base hand: mesh, skin, skeleton, poses). Read back by
// fbx_read.c, Blender and Maya (the test vr_base_hand)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct FbxW FbxW;

FbxW* fbxw_new(void);
void fbxw_free(FbxW* w);
// A node opened (its properties next, then its children), closed by fbxw_close
void fbxw_open(FbxW* w, const char* name);
void fbxw_close(FbxW* w);
// The open node ends with a nested list even without children (its null record): the FBX SDK's AnimationLayer needs it -
// without it Maya dropped the layer and every curve under it
void fbxw_block(FbxW* w);
// Properties of the open node (before its first child)
void fbxw_i(FbxW* w, int32_t v);   // I
void fbxw_l(FbxW* w, int64_t v);   // L
void fbxw_d(FbxW* w, double v);    // D
void fbxw_c(FbxW* w, uint8_t v);   // C
void fbxw_s(FbxW* w, const char* s);                // S
void fbxw_sn(FbxW* w, const char* s, uint32_t n);   // S with NULs (an object's "Name\0\1Class")
void fbxw_raw(FbxW* w, const void* p, uint32_t n);  // R
void fbxw_ad(FbxW* w, const double* v, uint32_t n); // d[]
void fbxw_af(FbxW* w, const float* v, uint32_t n);  // f[]
void fbxw_ai(FbxW* w, const int32_t* v, uint32_t n); // i[]
void fbxw_al(FbxW* w, const int64_t* v, uint32_t n); // l[]
// An object's name property: "name\0\1class"
void fbxw_objname(FbxW* w, const char* name, const char* cls);
// Shorthands: a node with one property
void fbxw_node_i(FbxW* w, const char* name, int32_t v);
void fbxw_node_s(FbxW* w, const char* name, const char* v);
void fbxw_node_d(FbxW* w, const char* name, double v);
// Properties70's P entries ("P": name, type, label, flags, values)
void fbxw_p_int(FbxW* w, const char* name, int32_t v);
void fbxw_p_enum(FbxW* w, const char* name, int32_t v);
void fbxw_p_double(FbxW* w, const char* name, double v);
void fbxw_p_ktime(FbxW* w, const char* name, int64_t v);
void fbxw_p_str(FbxW* w, const char* name, const char* v);
void fbxw_p_vec(FbxW* w, const char* name, const char* type, const char* label, const char* flags, double x, double y, double z);
// The whole file (header, the top nodes, footer); false: the file could not be written or a node is still open
bool fbxw_save(FbxW* w, const char* path);
