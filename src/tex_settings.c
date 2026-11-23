// (2026-11-23) A texture's own settings (tex_settings.h)
#include "tex_settings.h"

#include "io_json.h"
#include "material.h"
#include "platform.h"
#include "platform_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_COLORSPACE_SRGB 0 // (the Image Texture node's, shaders/tl/kernel_svm_types.h)
#define IMAGE_COLORSPACE_NONCOLOR 1

// the disk path of a "/Content/..." one, else as it is
static void tex_disk(const char* path, char* out, size_t cap) {
    const char* root = mat_root();
    if (!strncmp(path, "/Content", 8) && root && root[0]) snprintf(out, cap, "%s%s", root, path + 8);
    else snprintf(out, cap, "%s", path);
}
// its hidden file: <folder>/.<name>.rtex
static void tex_side(const char* disk, char* out, size_t cap) {
    const char* bn = disk + strlen(disk);
    while (bn > disk && bn[-1] != '/' && bn[-1] != '\\') bn--;
    snprintf(out, cap, "%.*s.%s.rtex", (int)(bn - disk), disk, bn);
}

void tex_settings_by_name(const char* path, TexSettings* out) {
    memset(out, 0, sizeof *out);
    int slot = mat_map_slot(path, NULL, 0);
    if (slot == MAT_MAP_NORMAL || slot == MAT_MAP_NORMAL_DX) out->color = TEXC_NORMAL;
    else if (slot != MAT_MAP_NONE && slot != MAT_MAP_BASE && slot != MAT_MAP_EMIT) out->color = TEXC_LINEAR; // (data: roughness, metallic, ORM, AO, height, opacity, gloss)
}

void tex_settings_get(const char* path, TexSettings* out) {
    tex_settings_by_name(path ? path : "", out);
    if (!path || !path[0]) return;
    char disk[1100], side[1200];
    tex_disk(path, disk, sizeof disk);
    tex_side(disk, side, sizeof side);
    FILE* f = re_fopen(side, "rb");
    if (!f) return;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    JsonDoc doc;
    if (!json_parse(buf, n, &doc, NULL, 0)) return;
    const char* c = json_get_str(&doc.root, "color", "");
    out->color = !strcmp(c, "linear") ? TEXC_LINEAR : !strcmp(c, "normal") ? TEXC_NORMAL : !strcmp(c, "srgb") ? TEXC_SRGB : out->color;
    out->compression = !strcmp(json_get_str(&doc.root, "compression", "bc7"), "none") ? TEXZ_NONE : TEXZ_BC7;
    out->filter = (uint8_t)json_get_num(&doc.root, "filter", 0) & 1;
    out->wrap = (uint8_t)json_get_num(&doc.root, "wrap", 0) & 3;
    double ms = json_get_num(&doc.root, "max_size", 0);
    out->max_size = (uint16_t)(ms > 0 && ms <= 16384 ? ms : 0);
    out->saved = true;
    json_free(&doc);
}

bool tex_settings_save(const char* path, const TexSettings* s) {
    char disk[1100], side[1200];
    tex_disk(path, disk, sizeof disk);
    tex_side(disk, side, sizeof side);
    FILE* f = re_fopen(side, "wb");
    if (!f) return false;
    static const char* const C[TEXC_COUNT] = { "srgb", "linear", "normal" };
    int ok = fprintf(f, "{ \"version\": 1, \"color\": \"%s\", \"compression\": \"%s\", \"filter\": %d, \"wrap\": %d, \"max_size\": %d }\n",
                     C[s->color % TEXC_COUNT], s->compression == TEXZ_NONE ? "none" : "bc7", s->filter, s->wrap, s->max_size);
    return fclose(f) == 0 && ok > 0;
}

void tex_settings_to_node(const char* path, struct MatNode* n) {
    TexSettings s;
    tex_settings_get(path, &s);
    n->prop[0] = s.filter, n->prop[1] = s.wrap;
    n->prop[2] = s.color == TEXC_SRGB ? IMAGE_COLORSPACE_SRGB : IMAGE_COLORSPACE_NONCOLOR;
}
