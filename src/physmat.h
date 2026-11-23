// Physical Materials (Q8, notas-dev/fisica.md "Materiais físicos"): Unreal's Physical Material as an asset (.rphysmat,
// a small JSON): friction, restitution (bounce), density and how two of them combine. A body's Details > Physics >
// Physical Material (CRigidBody.phys_material: the path's hash) overrides its own Friction / Restitution, and its mass
// comes from the density when Mass is 0. Read once, again when the file changes (its time); nothing in the frame.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// How a pair's values meet (Unreal's EFrictionCombineMode): the pair uses the higher of the two modes (Max wins)
enum { PCOMB_AVERAGE = 0, PCOMB_MIN, PCOMB_MULTIPLY, PCOMB_MAX, PCOMB_COUNT };
extern const char* const g_phys_combine_names[PCOMB_COUNT];

typedef struct PhysMaterial {
    float friction;            // 0 ice .. 1 rubber
    float restitution;         // 0 no bounce .. 1 all the speed back
    float density;             // g/cm3 (water 1, wood 0.6, steel 7.8): the mass when the body's Mass is 0
    uint32_t friction_combine, restitution_combine; // PCOMB_
    float static_friction;     // (reserved: as Unreal's, the friction at rest; 0: the same)
} PhysMaterial;

uint32_t physmat_vpath_hash(const char* vpath);    // CRigidBody.phys_material of "/Content/....rphysmat"
const PhysMaterial* physmat_get(uint32_t hash);    // NULL: none or not found (the body's own fields then)
const char* physmat_vpath(uint32_t hash);          // the path of a material that was read ("": unknown)
bool physmat_is_path(const char* vpath);
void physmat_defaults(PhysMaterial* m);
bool physmat_load(const char* vpath, PhysMaterial* m);
bool physmat_save(const char* vpath, const PhysMaterial* m);
bool physmat_create(const char* folder_vpath, const char* name, char* out_vpath, uint32_t cap); // a new one (named)
void physmat_reload(void);                          // the files read again (an edit, an import)
void physmat_remember(const char* vpath);           // a path known now (its hash found without looking through Content)
// The Physical Material editor (a small window like Unreal's: its Details); a test opens one
void physmat_editor_open(const char* vpath);
void physmat_editor_draw(void);
bool physmat_editor_add_menu(const char* folder_vpath); // Content Browser > Add > Physics > Physical Material (a preset)
// The presets (Content Browser > Add > Physics > Physical Material and the editor's Preset menu)
typedef struct PhysMatPreset { const char* name; PhysMaterial m; } PhysMatPreset;
#define PHYSMAT_PRESETS 8
extern const PhysMatPreset g_physmat_presets[PHYSMAT_PRESETS];
