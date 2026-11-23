// A packaged game's identity in its executable (sugestão #37, notas-dev/jogo.md): its icon and its version, written
// into the copy of realengine_game.exe by Windows' resource update (BeginUpdateResource), so Explorer, the taskbar,
// the window (GLFW loads the icon named GLFW_ICON) and the file's Properties show the game's, not the engine's.
//   * The icon: a .ico as it is (its images), or a picture (.png, .jpg, .bmp, .tga) made into 256, 64, 48, 32 and 16 px
//     32-bit images (box-filtered, the picture fitted in the square with transparent margins).
//   * The version: Company, Product, Description, Version (its numbers in the fixed part too), Copyright, the file name.
// The engine's own icon and version (data/branding/realengine.rc.in) are removed first, in the language they were
// compiled with, so only the game's are left.
#include "package_res.h"
#include "platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

// (wide resource types: the file is built without UNICODE)
#define RTW_ICON MAKEINTRESOURCEW(3)
#define RTW_GROUP_ICON MAKEINTRESOURCEW(14)
#define RTW_VERSION MAKEINTRESOURCEW(16)

uint8_t* mat_load_image_rgba(const char* path, uint32_t* w, uint32_t* h); // (mat_image.c)

typedef struct Buf {
    uint8_t* p;
    size_t n, cap;
} Buf;

static bool put(Buf* b, const void* d, size_t n) {
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096;
        while (c < b->n + n) c *= 2;
        uint8_t* q = realloc(b->p, c);
        if (!q) return false;
        b->p = q, b->cap = c;
    }
    if (d) memcpy(b->p + b->n, d, n);
    else memset(b->p + b->n, 0, n);
    b->n += n;
    return true;
}
static void put16(Buf* b, uint16_t v) { put(b, &v, 2); }
static void put32(Buf* b, uint32_t v) { put(b, &v, 4); }
static void align4(Buf* b) {
    while (b->n & 3) put(b, NULL, 1);
}

// --- The icon ---

#pragma pack(push, 2)
typedef struct IcoDirEntry { // a .ico's entry
    uint8_t w, h, colors, reserved;
    uint16_t planes, bits;
    uint32_t bytes, offset;
} IcoDirEntry;
typedef struct GrpEntry { // RTW_GROUP_ICON's entry: the image by its RTW_ICON id
    uint8_t w, h, colors, reserved;
    uint16_t planes, bits;
    uint32_t bytes;
    uint16_t id;
} GrpEntry;
#pragma pack(pop)

#define MAX_ICON_IMAGES 16
typedef struct IconSet {
    int n;
    Buf img[MAX_ICON_IMAGES];
    GrpEntry e[MAX_ICON_IMAGES];
} IconSet;

static void iconset_free(IconSet* s) {
    for (int i = 0; i < s->n; i++) free(s->img[i].p);
    memset(s, 0, sizeof *s);
}

// A size x size 32-bit icon image (a DIB: the header, BGRA bottom-up, an empty AND mask) of the picture fitted in it
static bool dib_icon(Buf* out, const uint8_t* rgba, uint32_t w, uint32_t h, int size) {
    float scale = (float)size / (float)(w > h ? w : h);
    int dw = (int)(w * scale + 0.5f), dh = (int)(h * scale + 0.5f);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int ox = (size - dw) / 2, oy = (size - dh) / 2;
    BITMAPINFOHEADER bi = { sizeof bi, size, size * 2, 1, 32, BI_RGB, 0, 0, 0, 0, 0 };
    uint32_t mask_row = ((uint32_t)size + 31) / 32 * 4;
    bi.biSizeImage = (DWORD)(size * size * 4 + mask_row * size);
    if (!put(out, &bi, sizeof bi)) return false;
    for (int y = size - 1; y >= 0; y--) // (bottom-up)
        for (int x = 0; x < size; x++) {
            float acc[4] = { 0, 0, 0, 0 };
            int dx = x - ox, dy = y - oy;
            if (dx >= 0 && dy >= 0 && dx < dw && dy < dh) { // the source texels under this pixel, averaged (alpha-weighted)
                uint32_t x0 = (uint32_t)((float)dx / scale), x1 = (uint32_t)((float)(dx + 1) / scale);
                uint32_t y0 = (uint32_t)((float)dy / scale), y1 = (uint32_t)((float)(dy + 1) / scale);
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                if (x1 > w) x1 = w;
                if (y1 > h) y1 = h;
                float cnt = 0;
                for (uint32_t sy = y0; sy < y1; sy++)
                    for (uint32_t sx = x0; sx < x1; sx++) {
                        const uint8_t* s = rgba + ((size_t)sy * w + sx) * 4;
                        float a = s[3] / 255.0f;
                        acc[0] += s[0] * a, acc[1] += s[1] * a, acc[2] += s[2] * a, acc[3] += s[3];
                        cnt += 1;
                    }
                if (cnt > 0 && acc[3] > 0) {
                    float a = acc[3] / cnt;
                    for (int k = 0; k < 3; k++) acc[k] = acc[k] / cnt / (a / 255.0f);
                    acc[3] = a;
                } else acc[0] = acc[1] = acc[2] = acc[3] = 0;
            }
            uint8_t px[4] = { (uint8_t)(acc[2] > 255 ? 255 : acc[2]), (uint8_t)(acc[1] > 255 ? 255 : acc[1]),
                              (uint8_t)(acc[0] > 255 ? 255 : acc[0]), (uint8_t)(acc[3] + 0.5f) };
            if (!put(out, px, 4)) return false;
        }
    return put(out, NULL, (size_t)mask_row * size); // (all 0: the alpha decides)
}

static bool load_icon(const char* path, IconSet* s, char* err, size_t cap) {
    memset(s, 0, sizeof *s);
    const char* dot = strrchr(path, '.');
    if (dot && !_stricmp(dot, ".ico")) { // its images as they are
        FILE* f = re_fopen(path, "rb");
        if (!f) return snprintf(err, cap, "cannot read %s", path), false;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t* d = n > 6 ? malloc((size_t)n) : NULL;
        bool ok = d && fread(d, 1, (size_t)n, f) == (size_t)n;
        fclose(f);
        uint16_t count = ok ? (uint16_t)(d[4] | d[5] << 8) : 0;
        if (!ok || d[2] != 1 || !count || 6 + (size_t)count * sizeof(IcoDirEntry) > (size_t)n) {
            free(d);
            return snprintf(err, cap, "%s is not an icon", path), false;
        }
        for (int i = 0; i < count && s->n < MAX_ICON_IMAGES; i++) {
            IcoDirEntry e;
            memcpy(&e, d + 6 + i * sizeof e, sizeof e);
            if ((size_t)e.offset + e.bytes > (size_t)n) continue;
            if (!put(&s->img[s->n], d + e.offset, e.bytes)) break;
            s->e[s->n] = (GrpEntry){ e.w, e.h, e.colors, 0, e.planes, e.bits, e.bytes, (uint16_t)(s->n + 1) };
            s->n++;
        }
        free(d);
        return s->n > 0 || (snprintf(err, cap, "%s has no images", path), false);
    }
    uint32_t w = 0, h = 0;
    uint8_t* rgba = mat_load_image_rgba(path, &w, &h);
    if (!rgba) return snprintf(err, cap, "cannot read the picture %s", path), false;
    static const int sizes[] = { 256, 64, 48, 32, 16 };
    for (int i = 0; i < 5; i++) {
        if (!dib_icon(&s->img[s->n], rgba, w, h, sizes[i])) break;
        uint8_t wh = (uint8_t)(sizes[i] >= 256 ? 0 : sizes[i]);
        s->e[s->n] = (GrpEntry){ wh, wh, 0, 0, 1, 32, (uint32_t)s->img[s->n].n, (uint16_t)(s->n + 1) };
        s->n++;
    }
    free(rgba);
    return s->n == 5 || (snprintf(err, cap, "out of memory"), false);
}

// --- The version (VS_VERSIONINFO: blocks of length, value length, type, key, then children; 32-bit aligned) ---

static size_t block_begin(Buf* b, const wchar_t* key, uint16_t value_len, uint16_t type) {
    size_t at = b->n;
    put16(b, 0), put16(b, value_len), put16(b, type);
    put(b, key, (wcslen(key) + 1) * sizeof(wchar_t));
    align4(b);
    return at;
}
static void block_end(Buf* b, size_t at) {
    uint16_t len = (uint16_t)(b->n - at);
    memcpy(b->p + at, &len, 2);
    align4(b);
}
static void string_value(Buf* b, const wchar_t* key, const char* utf8) {
    wchar_t v[256];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8 ? utf8 : "", -1, v, 256);
    if (n <= 0) v[0] = 0, n = 1;
    size_t at = block_begin(b, key, (uint16_t)n, 1);
    put(b, v, (size_t)n * sizeof(wchar_t));
    block_end(b, at);
}

static bool make_version(Buf* b, const PackageIdentity* id) {
    unsigned v[4] = { 0, 0, 0, 0 };
    sscanf(id->version && id->version[0] ? id->version : "1.0", "%u.%u.%u.%u", &v[0], &v[1], &v[2], &v[3]);
    VS_FIXEDFILEINFO fi = { 0 };
    fi.dwSignature = 0xFEEF04BD, fi.dwStrucVersion = 0x00010000;
    fi.dwFileVersionMS = fi.dwProductVersionMS = (v[0] & 0xFFFF) << 16 | (v[1] & 0xFFFF);
    fi.dwFileVersionLS = fi.dwProductVersionLS = (v[2] & 0xFFFF) << 16 | (v[3] & 0xFFFF);
    fi.dwFileFlagsMask = 0x3F, fi.dwFileOS = VOS_NT_WINDOWS32, fi.dwFileType = VFT_APP;
    size_t root = block_begin(b, L"VS_VERSION_INFO", sizeof fi, 0);
    put(b, &fi, sizeof fi);
    align4(b);
    size_t sfi = block_begin(b, L"StringFileInfo", 0, 1);
    size_t table = block_begin(b, L"040904b0", 0, 1); // (U.S. English, Unicode: what Explorer reads first)
    string_value(b, L"CompanyName", id->company);
    string_value(b, L"FileDescription", id->description && id->description[0] ? id->description : id->product);
    string_value(b, L"FileVersion", id->version);
    string_value(b, L"InternalName", id->product);
    string_value(b, L"LegalCopyright", id->copyright);
    string_value(b, L"OriginalFilename", id->file_name);
    string_value(b, L"ProductName", id->product);
    string_value(b, L"ProductVersion", id->version);
    block_end(b, table);
    block_end(b, sfi);
    size_t vfi = block_begin(b, L"VarFileInfo", 0, 1);
    size_t var = block_begin(b, L"Translation", 4, 0);
    put32(b, 0x04B00409u);
    block_end(b, var);
    block_end(b, vfi);
    block_end(b, root);
    return b->p != NULL;
}

// --- What the engine's executable has: its icon's language and image ids, its version's language ---

typedef struct Found {
    WORD icon_lang, ver_lang;
    bool icon, ver;
    uint16_t ids[64];
    int nids;
} Found;

static BOOL CALLBACK on_lang(HMODULE m, LPCWSTR type, LPCWSTR name, WORD lang, LONG_PTR user) {
    (void)m, (void)name;
    Found* f = (Found*)user;
    if (type == RTW_GROUP_ICON && !f->icon) f->icon = true, f->icon_lang = lang;
    if (type == RTW_VERSION && !f->ver) f->ver = true, f->ver_lang = lang;
    return TRUE;
}

static void find_existing(const wchar_t* exe, Found* f) {
    memset(f, 0, sizeof *f);
    HMODULE m = LoadLibraryExW(exe, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) return;
    EnumResourceLanguagesW(m, RTW_GROUP_ICON, L"GLFW_ICON", on_lang, (LONG_PTR)f);
    EnumResourceLanguagesW(m, RTW_VERSION, MAKEINTRESOURCEW(VS_VERSION_INFO), on_lang, (LONG_PTR)f);
    if (f->icon) { // the images the group names
        HRSRC r = FindResourceExW(m, RTW_GROUP_ICON, L"GLFW_ICON", f->icon_lang);
        HGLOBAL g = r ? LoadResource(m, r) : NULL;
        const uint8_t* d = g ? LockResource(g) : NULL;
        DWORD sz = r ? SizeofResource(m, r) : 0;
        if (d && sz >= 6) {
            int n = d[4] | d[5] << 8;
            for (int i = 0; i < n && f->nids < 64 && 6 + (size_t)(i + 1) * sizeof(GrpEntry) <= sz; i++) {
                GrpEntry e;
                memcpy(&e, d + 6 + i * sizeof e, sizeof e);
                f->ids[f->nids++] = e.id;
            }
        }
    }
    FreeLibrary(m);
}

bool package_set_identity(const char* exe_utf8, const PackageIdentity* id, char* err, size_t cap) {
    wchar_t exe[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, exe_utf8, -1, exe, 1024)) return snprintf(err, cap, "bad path"), false;
    IconSet icons;
    memset(&icons, 0, sizeof icons);
    if (id->icon_path && id->icon_path[0] && !load_icon(id->icon_path, &icons, err, cap)) return false;
    Buf ver = { 0 };
    if (!make_version(&ver, id)) {
        iconset_free(&icons);
        return snprintf(err, cap, "out of memory"), false;
    }
    Found f;
    find_existing(exe, &f);
    HANDLE h = BeginUpdateResourceW(exe, FALSE);
    bool ok = h != NULL;
    if (ok && icons.n) { // the engine's icon out, the game's in (as GLFW_ICON: the window's and Explorer's)
        WORD lang = f.icon ? f.icon_lang : MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
        for (int i = 0; i < f.nids; i++) UpdateResourceW(h, RTW_ICON, MAKEINTRESOURCEW(f.ids[i]), lang, NULL, 0);
        if (f.icon) UpdateResourceW(h, RTW_GROUP_ICON, L"GLFW_ICON", lang, NULL, 0);
        Buf grp = { 0 };
        put16(&grp, 0), put16(&grp, 1), put16(&grp, (uint16_t)icons.n);
        for (int i = 0; i < icons.n; i++) put(&grp, &icons.e[i], sizeof icons.e[i]);
        for (int i = 0; ok && i < icons.n; i++)
            ok = UpdateResourceW(h, RTW_ICON, MAKEINTRESOURCEW(icons.e[i].id), lang, icons.img[i].p, (DWORD)icons.img[i].n) != 0;
        ok = ok && grp.p && UpdateResourceW(h, RTW_GROUP_ICON, L"GLFW_ICON", lang, grp.p, (DWORD)grp.n);
        free(grp.p);
    }
    if (ok) { // the version: the engine's out, the game's in
        WORD lang = f.ver ? f.ver_lang : MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
        if (f.ver) UpdateResourceW(h, RTW_VERSION, MAKEINTRESOURCEW(VS_VERSION_INFO), lang, NULL, 0);
        ok = UpdateResourceW(h, RTW_VERSION, MAKEINTRESOURCEW(VS_VERSION_INFO), lang, ver.p, (DWORD)ver.n) != 0;
    }
    if (h && !EndUpdateResourceW(h, ok ? FALSE : TRUE)) ok = false;
    if (!ok) snprintf(err, cap, "could not write the resources (error %lu)", (unsigned long)GetLastError());
    free(ver.p);
    iconset_free(&icons);
    return ok;
}

int package_icon_count(const char* exe_utf8) {
    wchar_t exe[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, exe_utf8, -1, exe, 1024)) return 0;
    Found f;
    find_existing(exe, &f);
    return f.nids;
}
