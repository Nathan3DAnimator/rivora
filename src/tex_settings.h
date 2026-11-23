// (2026-11-23, the user: "a texture opened: RGB / non-color / linear, its quality; imported, set by itself from its name")
// A texture's own settings (Unreal's Texture asset, simpler), in a hidden file beside it (".<file>.rtex", as the
// meshes' ".<mesh>.mat"): its color space, the largest size the materials read it at, its compression, its filter and
// its wrap. With no file they come from its name (Roughness, Metallic, AO, Height, Opacity: Linear; Normal: Normal Map;
// the rest sRGB). The Image Texture nodes made with it take them; the material bakes read it at its size and compression.
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { TEXC_SRGB = 0, TEXC_LINEAR, TEXC_NORMAL, TEXC_COUNT };       // color space: color, data (Non-Color), a normal map
enum { TEXZ_BC7 = 0, TEXZ_NONE, TEXZ_COUNT };                       // compression: BC7 (a quarter of the memory), none
typedef struct TexSettings {
    uint8_t color;        // TEXC_*
    uint8_t compression;  // TEXZ_*
    uint8_t filter;       // the Image Texture node's interpolation (0 Linear, 1 Closest)
    uint8_t wrap;         // its extension (0 Repeat, 1 Extend, 2 Clip, 3 Mirror)
    uint16_t max_size;    // the largest side the materials read it at (0: as it is)
    bool saved;           // from its file (false: from its name)
} TexSettings;

// path: a disk path or a "/Content/..." one (under mat_root). Always fills out (its file's, else by its name)
void tex_settings_get(const char* path, TexSettings* out);
bool tex_settings_save(const char* path, const TexSettings* s);
void tex_settings_by_name(const char* path, TexSettings* out); // (what its name says)
struct MatNode;
void tex_settings_to_node(const char* path, struct MatNode* n); // an Image Texture node's color space, filter, wrap from them
