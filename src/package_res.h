// A packaged game's identity in its executable (package_res.c, sugestão #37): its icon and its version info.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct PackageIdentity {
    const char* icon_path;   // a .ico, or a picture (.png, .jpg, .bmp, .tga) made into its sizes; NULL / "": the engine's
    const char* product;     // the game's name
    const char* company;
    const char* version;     // "1.2" or "1.2.3.4"
    const char* description;
    const char* copyright;
    const char* file_name;   // "Game.exe"
} PackageIdentity;

// Writes them into the executable (a copy of realengine_game.exe); false with the reason in err
bool package_set_identity(const char* exe, const PackageIdentity* id, char* err, size_t cap);
// The images of its window icon (GLFW_ICON): the check of the tests
int package_icon_count(const char* exe);
