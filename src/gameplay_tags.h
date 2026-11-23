// Gameplay Tags (Unreal's, the light version: notas-dev/blueprints.md "Gameplay Tags"): hierarchical names separated by
// dots ("Damage.Fire", "State.Stunned") on an actor's Gameplay Tags component (CGameplayTags). A query matches a tag
// that is it or under it: an actor tagged "Damage.Fire" has "Damage" (Unreal's HasTag); exact: only "Damage.Fire".
// No allocation: the tags live in the component; matching compares the strings.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

// tag is query, or query followed by '.' and more (exact: equal only). Case-insensitive like Unreal's FName
bool gt_matches(const char* tag, const char* query, bool exact);
bool gt_has(World* w, Entity e, const char* query, bool exact); // false: no component / no such tag
// Adds the tag (the component made when missing; a tag already there is kept once). False: full / invalid name
bool gt_add(World* w, Entity e, const char* tag);
bool gt_remove(World* w, Entity e, const char* tag); // the exact tag; false: it wasn't there
// A tag's name cleaned: spaces trimmed, no leading / trailing / doubled dots. False when nothing is left
bool gt_clean(const char* in, char* out, size_t cap);
int gt_selftest(void); // matching, add / remove on a world (--tags-test); failures

// The project's tag list (Project Settings > Gameplay Tags; Config/DefaultGameplayTags.ini like Unreal's): the tags a
// picker offers, sorted (the dots make the tree). Read again when another project opens; every change is saved
#define GT_PROJECT_MAX 1024
uint32_t gt_project_count(void);
const char* gt_project_at(uint32_t i);
int gt_project_index(const char* tag); // -1: not in the list
bool gt_project_add(const char* tag);  // cleaned; true when it is there afterwards
// A tag renamed with every tag under it, in the list and on the level's actors; how many were renamed
uint32_t gt_project_rename(World* w, const char* from, const char* to);
uint32_t gt_project_remove(const char* tag);     // the tag and those under it from the list (the actors keep theirs)
uint32_t gt_project_import_level(World* w);      // the level's actors' tags the list lacks, added; how many
void gt_project_reset(void);                     // (the tests: the list read again from the project)
// The actors (not components) that have the tag: exact, or under it like Has Matching. Up to cap into out; the count
// (all of them, also past cap). Get All Actors with Tag / with Gameplay Tag
uint32_t gt_find_actors(World* w, const char* query, bool exact, Entity* out, uint32_t cap);
// (editor, gameplay_tags_ui.c) A tag chosen from the project's list: a button with the tag, a popup with a search box and
// the tree; a name typed that the list lacks can be added. True when the tag changed
bool gt_tag_picker(const char* id, char* tag, size_t cap, float width);
void gt_settings_page(World* w); // Project Settings > Gameplay Tags: the tree, add, rename, delete, the level's tags
