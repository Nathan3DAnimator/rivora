// The Tlender's .tlend into the Real Engine (tlend_import.c, notas-dev/importar-tlender.md): its meshes become Static
// Meshes (.rmesh), its materials are rebuilt from their nodes (.rmat: the same Blender idnames), an armature with the
// mesh it deforms becomes a Skeletal Mesh (.rskm, a part per bone) and each action an Animation Sequence (.ranim).
// The objects are placed in the level where the Tlender had them (Blender's meters and axes -> centimeters, Y mirrored).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "ecs.h"

#define TLEND_MAX_MAT_PATHS 128
typedef struct TlendReport {
    unsigned meshes, materials, skeletal, animations, actors, textures, skipped_nodes, skipped_rigs, lights, cameras;
    unsigned morphs; // morph targets kept (shape keys, glTF's targets): in Skeletal Meshes' skins
    unsigned grooms; // (2026-11-16) hair: the Tlender's hair objects, an OBJ's polylines (.rgroom guides and a Groom)
    char text[512];
    // the materials written (read again by the main thread when loaded: the importers run on a worker)
    unsigned nmat_paths;
    char mat_paths[TLEND_MAX_MAT_PATHS][600];
} TlendReport;

// src: the .tlend file; folder_vpath: where the assets go ("/Content/Bonnie"); w: the level (NULL: assets only)
bool tlend_import(const char* src, const char* folder_vpath, const char* content_root, World* w, TlendReport* r);
// (2026-11-23) A dropped folder being imported: its files are mirrored from src_root into dst_vroot (Content), so a
// texture a model uses from inside it is that mirrored one, not a copy in the model's Textures (NULL: off)
void tlend_import_set_mirror(const char* src_root, const char* dst_vroot); // (this thread's)
void tlend_import_get_mirror(char* src_root, size_t scap, char* dst_vroot, size_t dcap);

// An OBJ (and the MTL it names) the same way: its objects ("o", or "g" without any) as Static Meshes per material, the
// MTL's materials as Principled BSDF graphs (Kd and map_Kd, Ns/Pr, Pm, d/Tr), the actors placed at the origin
bool obj_import(const char* src, const char* folder_vpath, const char* content_root, World* w, TlendReport* r);

// A glTF 2.0 (.gltf with its .bin and images, or a .glb) the same way: a Static Mesh per mesh node and material at the
// node's world transform, the materials (metallic-roughness, normal map, emission) as Principled BSDF graphs, the
// embedded images written to <folder>/Textures (import_gltf.inc)
bool gltf_import(const char* src, const char* folder_vpath, const char* content_root, World* w, TlendReport* r);

// An OBJ (and the MTL it names) the same way: its objects ("o", or "g" without any) as Static Meshes per material, the
// MTL's materials as Principled BSDF graphs (Kd and map_Kd, Ns/Pr, Pm, d/Tr), the actors placed at the origin
bool obj_import(const char* src, const char* folder_vpath, const char* content_root, World* w, TlendReport* r);

// An FBX (binary 7.x or ASCII) the same way: static meshes per material, skinned meshes as a Skeletal Mesh per skeleton with
// its animation stacks baked, Phong/Lambert materials as Principled BSDF graphs, embedded textures written out (import_fbx.inc)
bool fbx_import(const char* src, const char* folder_vpath, const char* content_root, World* w, TlendReport* r);
// (#164) The exchange format (.rasset: RealEngine.md, the Tlender's Export > Real Engine) the same way: meshes, PBR materials
// with the images it carries, skeletons and skins as Skeletal Meshes, animations, morph targets, lights and cameras
bool rasset_import(const char* src, const char* folder_vpath, const char* content_root, World* w, TlendReport* r);

// Auto Rig (autorig.inc): a Static Mesh actor of a person as a Skeletal Mesh on the Mannequin's skeleton (its parts into
// folder_vpath, a new actor where it stood, the statue hidden); the test's statue (a Mannequin's parts as one .rmesh)
bool autorig_humanoid(World* w, Entity actor, const char* folder_vpath, const char* content_root, TlendReport* r);
bool autorig_test_statue(const char* disk, const char* body, bool tpose);
