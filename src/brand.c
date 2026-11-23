// The intro and the loading screen (brand.h)
#define COBJMACROS
#include "brand.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#define RE_BRAND_VIDEO 1 // (Media Foundation: the Android game shows the project's picture, not its video)
#endif

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <cimgui_impl.h>

#include "crashlog.h" // (the loading screen beats the hang watchdog)
#include "i18n.h"
#include "platform.h"
#include "render_gpu.h"
#include "vkctx.h"

// the embedded pictures (data/branding, CMake's bin2c)
#include "brand_logo.inc"
#include "brand_splash.inc"

#ifndef RE_VERSION_STRING
#define RE_VERSION_STRING "dev"
#endif

uint8_t* mat_load_image_rgba(const char* path, uint32_t* w, uint32_t* h);                   // mat_image.c (WIC)
uint8_t* mat_load_image_rgba_mem(const void* data, size_t n, uint32_t* w, uint32_t* h);

bool g_brand_intro_on = true, g_brand_splash_on = true;

typedef struct BrandTex {
    GpuImage img;
    VkDescriptorSet set; // ImGui's
    uint32_t w, h;
} BrandTex;

static struct {
    VkCtx* vk;
    BrandTex logo, splash, custom; // custom: the project's picture (or the video's frame)
    bool ready, has_custom;
    // the video (Media Foundation): its reader, its size, the time of the frame shown, the frame's pixels
#ifdef RE_BRAND_VIDEO
    IMFSourceReader* reader;
#else
    void* reader;
#endif
    uint32_t vw, vh;
    int32_t stride; // the type's default stride (0: unknown)
    double video_t0, frame_time;
    uint8_t* frame_px;
    GpuBuf staging;
    double splash_t0;
    float shown; // the progress drawn (eased toward the real one)
} B;

static BrandTex g_mark; // (Q26) the logo as a mark for the menu bar (brand_mark)
// UMG's pictures (brand_ui_image)
static struct { char vpath[256]; BrandTex t; bool failed; } g_ui_img[32];
// the run time's pictures (a save's: save_progress.h): by key, uploaded again when their generation changes
static struct { char key[80]; BrandTex t; uint32_t gen; bool used; } g_rt_img[8];
static uint32_t g_rt_next;
static uint32_t g_ui_nimg;

// --- Textures ---

static void tex_free(VkCtx* vk, BrandTex* t) {
    if (t->set && igGetCurrentContext()) ImGui_ImplVulkan_RemoveTexture(t->set); // (no UI left: its pool went with it)
    gpu_image_destroy(vk, &t->img);
    memset(t, 0, sizeof *t);
}

// RGBA8 pixels into a sampled image (through a staging buffer: an upload at startup or on a new picture)
static bool tex_upload(VkCtx* vk, BrandTex* t, const uint8_t* px, uint32_t w, uint32_t h) {
    bool fresh = !t->img.image || t->w != w || t->h != h;
    if (fresh) {
        tex_free(vk, t);
        if (!gpu_image_create(vk, &t->img, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
            return false;
        t->w = w, t->h = h;
    }
    VkDeviceSize n = (VkDeviceSize)w * h * 4;
    if (B.staging.size < n) {
        gpu_buf_destroy(vk, &B.staging);
        if (!gpu_buf_create(vk, &B.staging, n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
    }
    memcpy(B.staging.mapped, px, (size_t)n);
    VkCommandBuffer cmd = gpu_begin_once(vk);
    gpu_transition(cmd, &t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true);
    VkBufferImageCopy r = { 0 };
    r.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.imageExtent = (VkExtent3D){ w, h, 1 };
    vkCmdCopyBufferToImage(cmd, B.staging.buffer, t->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    gpu_transition(cmd, &t->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false);
    gpu_end_once(vk, cmd);
    if (fresh) t->set = ImGui_ImplVulkan_AddTexture(t->img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return t->set != VK_NULL_HANDLE;
}

static bool tex_from_memory(VkCtx* vk, BrandTex* t, const void* data, size_t n) {
    uint32_t w = 0, h = 0;
    uint8_t* px = mat_load_image_rgba_mem(data, n, &w, &h);
    bool ok = px && tex_upload(vk, t, px, w, h);
    free(px);
    return ok;
}

bool brand_init(VkCtx* vk) {
    B.vk = vk;
    B.ready = tex_from_memory(vk, &B.logo, g_brand_logo, sizeof g_brand_logo) &&
              tex_from_memory(vk, &B.splash, g_brand_splash, sizeof g_brand_splash);
    return B.ready;
}

static void video_close(void) {
#ifdef RE_BRAND_VIDEO
    if (B.reader) IMFSourceReader_Release(B.reader), MFShutdown();
#endif
    B.reader = NULL;
    free(B.frame_px);
    B.frame_px = NULL;
}

void brand_shutdown(VkCtx* vk) {
    video_close();
    tex_free(vk, &B.logo);
    tex_free(vk, &B.splash);
    tex_free(vk, &B.custom);
    tex_free(vk, &g_mark);
    for (uint32_t i = 0; i < g_ui_nimg; i++)
        if (!g_ui_img[i].failed) tex_free(vk, &g_ui_img[i].t);
    g_ui_nimg = 0;
    for (int i = 0; i < 8; i++)
        if (g_rt_img[i].used) tex_free(vk, &g_rt_img[i].t), g_rt_img[i].used = false;
    gpu_buf_destroy(vk, &B.staging);
    B.ready = B.has_custom = false;
}

#ifdef RE_BRAND_VIDEO
// --- The project's video: Media Foundation's source reader, frames as 32-bit RGB (BGRX), looped ---

static bool video_open(const char* path) {
    wchar_t wp[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024)) return false;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    IMFAttributes* at = NULL;
    MFCreateAttributes(&at, 2);
    if (at) IMFAttributes_SetUINT32(at, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    HRESULT hr = MFCreateSourceReaderFromURL(wp, at, &B.reader);
    if (at) IMFAttributes_Release(at);
    if (FAILED(hr)) {
        B.reader = NULL;
        MFShutdown();
        return false;
    }
    IMFMediaType* mt = NULL;
    MFCreateMediaType(&mt);
    IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    hr = IMFSourceReader_SetCurrentMediaType(B.reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, mt);
    IMFMediaType_Release(mt);
    IMFMediaType* cur = NULL;
    if (FAILED(hr) || FAILED(IMFSourceReader_GetCurrentMediaType(B.reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
        video_close();
        return false;
    }
    UINT64 size = 0;
    IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_SIZE, &size);
    UINT32 stride = 0;
    B.stride = SUCCEEDED(IMFMediaType_GetUINT32(cur, &MF_MT_DEFAULT_STRIDE, &stride)) ? (LONG)(INT32)stride : 0;
    IMFMediaType_Release(cur);
    B.vw = (uint32_t)(size >> 32), B.vh = (uint32_t)(size & 0xFFFFFFFFu);
    if (!B.vw || !B.vh || B.vw > 8192 || B.vh > 8192) {
        video_close();
        return false;
    }
    B.frame_px = malloc((size_t)B.vw * B.vh * 4);
    B.video_t0 = re_time_seconds();
    B.frame_time = -1.0;
    return B.frame_px != NULL;
}

// The frame due now (the reader's timestamps against the clock since the splash began): decoded, BGRX -> RGBA, uploaded
static void video_update(void) {
    if (!B.reader) return;
    double now = re_time_seconds() - B.video_t0;
    if (B.frame_time >= 0.0 && now < B.frame_time) return;
    for (int tries = 0; tries < 8; tries++) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* smp = NULL;
        HRESULT hr = IMFSourceReader_ReadSample(B.reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &flags, &ts, &smp);
        if (FAILED(hr)) return;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { // looped
            PROPVARIANT pv;
            PropVariantInit(&pv);
            pv.vt = VT_I8;
            pv.hVal.QuadPart = 0;
            IMFSourceReader_SetCurrentPosition(B.reader, &GUID_NULL, &pv);
            B.video_t0 = re_time_seconds();
            now = 0.0;
            if (smp) IMFSample_Release(smp);
            continue;
        }
        if (!smp) continue;
        double st = (double)ts / 1e7;
        if (st + 0.05 < now) { // late: the next one
            IMFSample_Release(smp);
            continue;
        }
        // the rows may be padded (a decoder's pitch): through IMF2DBuffer when the buffer has one, else the type's stride
        IMFMediaBuffer* buf = NULL;
        if (SUCCEEDED(IMFSample_GetBufferByIndex(smp, 0, &buf))) {
            IMF2DBuffer* b2 = NULL;
            BYTE* p = NULL;
            LONG pitch = 0;
            DWORD len = 0;
            bool locked2d = SUCCEEDED(IMFMediaBuffer_QueryInterface(buf, &IID_IMF2DBuffer, (void**)&b2)) && SUCCEEDED(IMF2DBuffer_Lock2D(b2, &p, &pitch));
            if (!locked2d && SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &len))) pitch = B.stride ? B.stride : (LONG)(len / B.vh);
            if (p && pitch) {
                // BGRX -> RGBA (a negative pitch: bottom-up rows); a big video every k-th pixel (at most ~1920 wide: a
                // 4K frame uploaded whole would cost the frame)
                uint32_t k = (B.vw + 1919) / 1920, ow = B.vw / k, oh = B.vh / k;
                for (uint32_t y = 0; y < oh; y++) {
                    uint32_t sy = y * k;
                    const BYTE* row = p + (pitch > 0 ? (intptr_t)sy * pitch : (intptr_t)(B.vh - 1 - sy) * -pitch);
                    uint8_t* o = B.frame_px + (size_t)y * ow * 4;
                    for (uint32_t x = 0; x < ow; x++) {
                        const BYTE* q = row + (size_t)x * k * 4;
                        o[x * 4] = q[2], o[x * 4 + 1] = q[1], o[x * 4 + 2] = q[0], o[x * 4 + 3] = 255;
                    }
                }
                tex_upload(B.vk, &B.custom, B.frame_px, ow, oh);
            }
            if (locked2d) IMF2DBuffer_Unlock2D(b2);
            else if (p) IMFMediaBuffer_Unlock(buf);
            if (b2) IMF2DBuffer_Release(b2);
            IMFMediaBuffer_Release(buf);
        }
        IMFSample_Release(smp);
        B.frame_time = st;
        return;
    }
}

#else
static bool video_open(const char* path) {
    (void)path;
    return false;
}
static void video_update(void) {}
#endif

bool brand_set_splash(VkCtx* vk, const char* path) {
    video_close();
    B.has_custom = false;
    if (!path || !path[0]) return true;
    const char* dot = strrchr(path, '.');
    char ext[8] = { 0 };
    for (int k = 0; dot && k < 7 && dot[k + 1]; k++) ext[k] = (char)(dot[k + 1] | 0x20);
    bool video = !strcmp(ext, "mp4") || !strcmp(ext, "wmv") || !strcmp(ext, "mov") || !strcmp(ext, "avi") || !strcmp(ext, "mkv") ||
                 !strcmp(ext, "webm") || !strcmp(ext, "m4v");
    if (video) {
        if (!video_open(path)) return false;
        video_update();
        B.has_custom = B.custom.set != VK_NULL_HANDLE;
        return B.has_custom;
    }
    uint32_t w = 0, h = 0;
    uint8_t* px = mat_load_image_rgba(path, &w, &h);
    B.has_custom = px && tex_upload(vk, &B.custom, px, w, h);
    free(px);
    return B.has_custom;
}

bool brand_splash_custom(void) { return B.has_custom; }


void* brand_ui_image(const char* content_root, const char* vpath, float* aspect) {
    for (uint32_t i = 0; i < g_ui_nimg; i++)
        if (!strcmp(g_ui_img[i].vpath, vpath)) {
            if (g_ui_img[i].failed) return NULL;
            if (aspect) *aspect = g_ui_img[i].t.h ? (float)g_ui_img[i].t.w / (float)g_ui_img[i].t.h : 1.0f;
            return (void*)(uintptr_t)g_ui_img[i].t.set;
        }
    if (g_ui_nimg >= 32 || !B.vk) return NULL;
    char path[1024];
    snprintf(path, sizeof path, "%s%s", content_root, !strncmp(vpath, "/Content", 8) ? vpath + 8 : vpath);
    uint32_t w = 0, h = 0;
    uint8_t* px = mat_load_image_rgba(path, &w, &h);
    snprintf(g_ui_img[g_ui_nimg].vpath, sizeof g_ui_img[0].vpath, "%s", vpath);
    g_ui_img[g_ui_nimg].failed = !px || !tex_upload(B.vk, &g_ui_img[g_ui_nimg].t, px, w, h);
    free(px);
    return brand_ui_image(content_root, g_ui_img[g_ui_nimg++].vpath, aspect);
}

// (Q26) The logo as a mark for the menu bar: the white R and its orbit as alpha over a white image (the logo's black
// background gone), 64 x 64 by a box filter (drawn at ~30 px: no shimmer), tinted by the caller with the theme's accent
static bool g_mark_failed;
void* brand_mark(void) {
    if (g_mark.set) return (void*)(uintptr_t)g_mark.set;
    if (g_mark_failed || !B.vk) return NULL;
    uint32_t w = 0, h = 0;
    uint8_t* px = mat_load_image_rgba_mem(g_brand_logo, sizeof g_brand_logo, &w, &h);
    enum { M = 64 };
    static uint8_t out[M * M * 4];
    g_mark_failed = true;
    if (px && w >= M && h >= M) {
        for (uint32_t y = 0; y < M; y++)
            for (uint32_t x = 0; x < M; x++) {
                uint32_t x0 = x * w / M, x1 = (x + 1) * w / M, y0 = y * h / M, y1 = (y + 1) * h / M, n = 0, sum = 0;
                for (uint32_t sy = y0; sy < y1; sy++)
                    for (uint32_t sx = x0; sx < x1; sx++, n++) {
                        const uint8_t* p = px + ((size_t)sy * w + sx) * 4;
                        uint32_t m = p[0] > p[1] ? p[0] : p[1];
                        sum += (m > p[2] ? m : p[2]) * p[3] / 255u;
                    }
                uint32_t a = n ? sum / n : 0;
                a = a * 5u / 4u > 255u ? 255u : a * 5u / 4u; // (the logo's grays: a little more opaque)
                uint8_t* o = out + ((size_t)y * M + x) * 4;
                o[0] = o[1] = o[2] = 255, o[3] = (uint8_t)a;
            }
        g_mark_failed = !tex_upload(B.vk, &g_mark, out, M, M);
    }
    free(px);
    return g_mark.set ? (void*)(uintptr_t)g_mark.set : NULL;
}

void* brand_ui_image_rgb(const char* key, const uint8_t* rgb, uint32_t w, uint32_t h, uint32_t gen) {
    if (!key || !rgb || !w || !h || w * h > 512u * 512u || !B.vk) return NULL;
    int k = -1;
    for (int i = 0; i < 8 && k < 0; i++)
        if (g_rt_img[i].used && !strcmp(g_rt_img[i].key, key)) k = i;
    if (k >= 0 && g_rt_img[k].gen == gen && g_rt_img[k].t.set) return (void*)(uintptr_t)g_rt_img[k].t.set;
    if (k < 0) { // (a free one, else the oldest: the slot list shows 8 at most)
        for (int i = 0; i < 8 && k < 0; i++)
            if (!g_rt_img[i].used) k = i;
        if (k < 0) k = (int)(g_rt_next++ % 8u);
        snprintf(g_rt_img[k].key, sizeof g_rt_img[k].key, "%s", key);
        g_rt_img[k].used = true;
    }
    static uint8_t px[512 * 512 * 4];
    for (uint32_t i = 0; i < w * h; i++) px[i * 4] = rgb[i * 3], px[i * 4 + 1] = rgb[i * 3 + 1], px[i * 4 + 2] = rgb[i * 3 + 2], px[i * 4 + 3] = 255;
    g_rt_img[k].gen = gen;
    if (!tex_upload(B.vk, &g_rt_img[k].t, px, w, h)) return NULL; // (a new picture: a rare upload, the GPU waited for)
    return (void*)(uintptr_t)g_rt_img[k].t.set;
}

// --- Drawing helpers ---

static float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static float smooth(float a, float b, float x) {
    float t = clamp01((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}
static ImU32 rgba(float r, float g, float b, float a) {
    return ((ImU32)(clamp01(a) * 255.0f) << 24) | ((ImU32)(clamp01(b) * 255.0f) << 16) | ((ImU32)(clamp01(g) * 255.0f) << 8) |
           (ImU32)(clamp01(r) * 255.0f);
}
static float hash1(float n) { // a fixed pseudo-random 0..1 per n
    double v = sin((double)n * 12.9898) * 43758.5453;
    return (float)(v - floor(v));
}

// Text centered on x (font size px), with extra space between the letters (tracking, px)
static void text_spaced(ImDrawList* dl, float cx, float y, float size, float tracking, ImU32 col, const char* s) {
    ImFont* font = igGetFont();
    float w = 0.0f;
    for (const char* p = s; *p;) {
        const char* q = p + 1;
        while ((*q & 0xC0) == 0x80) q++;
        w += ImFont_CalcTextSizeA(font, size, 1e9f, 0.0f, p, q, NULL).x + tracking;
        p = q;
    }
    w -= tracking;
    float x = cx - w * 0.5f;
    for (const char* p = s; *p;) {
        const char* q = p + 1;
        while ((*q & 0xC0) == 0x80) q++;
        ImDrawList_AddText_FontPtr(dl, font, size, (ImVec2_c){ x, y }, col, p, q, 0.0f, NULL);
        x += ImFont_CalcTextSizeA(font, size, 1e9f, 0.0f, p, q, NULL).x + tracking;
        p = q;
    }
}
static void text_at(ImDrawList* dl, float x, float y, float size, ImU32 col, const char* s, int align) { // align: 0 left, 1 center, 2 right
    ImFont* font = igGetFont();
    float w = ImFont_CalcTextSizeA(font, size, 1e9f, 0.0f, s, NULL, NULL).x;
    ImDrawList_AddText_FontPtr(dl, font, size, (ImVec2_c){ x - (align == 1 ? w * 0.5f : align == 2 ? w : 0.0f), y }, col, s, NULL, 0.0f, NULL);
}

// --- The intro: what the engine is, in 3.4 s. A field of points comes together into a turning wireframe (the
// geometry), the light sweeps it into the polished R (the render), its orbit draws itself around (the world in
// motion), the name settles in, then all of it fades. ---

#define INTRO_LEN 3.4
static void intro_draw(double t, float w, float h) {
    ImDrawList* dl = igGetBackgroundDrawList(NULL);
    ImDrawList_AddRectFilled(dl, (ImVec2_c){ 0, 0 }, (ImVec2_c){ w, h }, 0xFF000000u, 0.0f, 0); // theme: literal (the intro's black: the brand's, not a theme's)
    float s = fminf(w, h), cx = w * 0.5f, cy = h * 0.46f, R = s * 0.22f;
    float fade_out = 1.0f - smooth(2.9f, (float)INTRO_LEN, (float)t), ft = (float)t;
    // the points: scattered, drawn in toward a sphere's surface around the logo (0 .. 1.1 s), then out of sight
    float gather = smooth(0.0f, 1.1f, ft), turn = ft * 0.9f;
    for (int i = 0; i < 160; i++) {
        float a = hash1((float)i * 1.7f) * 6.2832f, b = acosf(hash1((float)i * 3.1f) * 2.0f - 1.0f);
        float px = sinf(b) * cosf(a + turn), py = cosf(b), pz = sinf(b) * sinf(a + turn);
        float spread = 3.5f + hash1((float)i * 7.3f) * 4.0f, d = spread + (1.0f - spread) * gather;
        float x = cx + px * R * d * (1.0f / (1.6f - pz * 0.4f)) * 1.4f, y = cy + py * R * d * (1.0f / (1.6f - pz * 0.4f)) * 1.4f;
        float al = smooth(0.0f, 0.5f, ft) * (1.0f - smooth(1.2f, 1.8f, ft)) * (0.45f + 0.55f * (pz * 0.5f + 0.5f));
        ImDrawList_AddCircleFilled(dl, (ImVec2_c){ x, y }, 1.2f + pz * 0.6f, rgba(0.75f, 0.85f, 1.0f, al), 6);
    }
    // the wireframe: an icosphere's edges turning (geometry), its lines fading as the logo takes its place
    float wire = smooth(0.5f, 0.9f, ft) * (1.0f - smooth(1.3f, 1.9f, ft));
    if (wire > 0.0f) {
        static const float ico[12][3] = { { -1, 1.618f, 0 }, { 1, 1.618f, 0 }, { -1, -1.618f, 0 }, { 1, -1.618f, 0 },
                                          { 0, -1, 1.618f }, { 0, 1, 1.618f }, { 0, -1, -1.618f }, { 0, 1, -1.618f },
                                          { 1.618f, 0, -1 }, { 1.618f, 0, 1 }, { -1.618f, 0, -1 }, { -1.618f, 0, 1 } };
        ImVec2_c p2[12];
        float dep[12];
        for (int i = 0; i < 12; i++) {
            float x = ico[i][0] / 1.902f, y = ico[i][1] / 1.902f, z = ico[i][2] / 1.902f;
            float ca = cosf(turn * 1.3f), sa = sinf(turn * 1.3f), x2 = x * ca + z * sa, z2 = -x * sa + z * ca;
            float cb = cosf(0.4f), sb = sinf(0.4f), y2 = y * cb - z2 * sb, z3 = y * sb + z2 * cb;
            float k = R * 0.95f / (1.6f - z3 * 0.4f) * 1.4f;
            p2[i] = (ImVec2_c){ cx + x2 * k, cy + y2 * k };
            dep[i] = z3;
        }
        for (int i = 0; i < 12; i++)
            for (int j = i + 1; j < 12; j++) {
                float dx = ico[i][0] - ico[j][0], dy = ico[i][1] - ico[j][1], dz = ico[i][2] - ico[j][2];
                if (fabsf(dx * dx + dy * dy + dz * dz - 4.0f) > 0.1f) continue; // the 30 edges (length 2)
                float a = wire * (0.35f + 0.4f * ((dep[i] + dep[j]) * 0.25f + 0.5f));
                ImDrawList_AddLine(dl, p2[i], p2[j], rgba(0.55f, 0.75f, 1.0f, a), 1.4f);
            }
    }
    // the logo: revealed by a diagonal light sweep (1.0 .. 1.9 s), then held, gently breathing
    float reveal = smooth(1.0f, 1.9f, ft), L = s * 0.46f;
    if (reveal > 0.0f && B.logo.set) {
        float x0 = cx - L * 0.5f, y0 = cy - L * 0.5f;
        float edge = reveal * 1.25f; // the sweep's front (uv), from the lower left
        int strips = 48;
        for (int k = 0; k < strips; k++) { // vertical strips, each as revealed as the front has passed it
            float u0 = (float)k / strips, u1 = (float)(k + 1) / strips, ux = (u0 + u1) * 0.5f;
            float a = clamp01((edge - ux * 0.8f) * 6.0f) * fade_out;
            if (a <= 0.0f) continue;
            ImDrawList_AddImage(dl, (ImTextureRef_c){ NULL, (ImTextureID)(uintptr_t)B.logo.set }, (ImVec2_c){ x0 + u0 * L, y0 },
                                (ImVec2_c){ x0 + u1 * L, y0 + L }, (ImVec2_c){ u0, 0 }, (ImVec2_c){ u1, 1 }, rgba(1, 1, 1, a));
        }
        if (reveal < 1.0f) { // the light's front: a bright slanted bar
            float fx = x0 + clamp01(edge / 0.8f) * L;
            ImDrawList_AddQuadFilled(dl, (ImVec2_c){ fx - 6, y0 + L * 0.15f }, (ImVec2_c){ fx + 6, y0 + L * 0.15f },
                                     (ImVec2_c){ fx - 26, y0 + L * 0.85f }, (ImVec2_c){ fx - 38, y0 + L * 0.85f }, rgba(1, 1, 1, 0.35f));
        }
        // the glow: a flash when the sweep ends, then a slow breath
        float flash = smooth(1.75f, 1.95f, ft) * (1.0f - smooth(1.95f, 2.5f, ft)) * 0.5f + 0.05f * sinf(ft * 3.0f) * reveal;
        for (int r = 0; r < 6; r++)
            ImDrawList_AddCircleFilled(dl, (ImVec2_c){ cx + L * 0.12f, cy - L * 0.1f }, L * (0.18f + r * 0.07f),
                                       rgba(0.8f, 0.9f, 1.0f, flash * 0.12f * fade_out), 48);
    }
    // the name and what it does
    float name = smooth(1.9f, 2.5f, ft) * fade_out, tag = smooth(2.25f, 2.8f, ft) * fade_out;
    if (name > 0.0f) text_spaced(dl, cx, cy + L * 0.5f + s * 0.02f, s * 0.07f, s * 0.012f + (1.0f - name) * s * 0.03f, rgba(0.92f, 0.93f, 0.96f, name), "REAL ENGINE");
    if (tag > 0.0f) {
        char line[160];
        snprintf(line, sizeof line, "%s   \xe2\x80\xa2   %s   \xe2\x80\xa2   %s", TR("CREATE"), TR("RENDER"), TR("PLAY"));
        text_spaced(dl, cx, cy + L * 0.5f + s * 0.11f, s * 0.022f, s * 0.008f, rgba(0.7f, 0.74f, 0.8f, tag), line);
    }
}

#ifdef _WIN32
#include "brand_intro_win.inc" // (2026-11-23) the editor's intro over the desktop: a transparent window of its own
#endif

// With multi-viewport on (the editor's main.c), every ImGui frame must end with UpdatePlatformWindows before the next
// NewFrame (else ImGui asserts: the 0.8 closed at the intro); no window is out of the main one here, none is drawn.
static void end_platform_frame(void) {
    if (igGetIO_Nil()->ConfigFlags & ImGuiConfigFlags_ViewportsEnable) igUpdatePlatformWindows();
}

void brand_run_intro(VkCtx* vk, GLFWwindow* window) {
#ifdef _WIN32
    if (li_wait(window)) return; // (the editor's: playing in its own window since brand_intro_start)
#endif
    if (!g_brand_intro_on || !B.ready) return;
    double t0 = re_time_seconds();
    bool skip = false;
    while (!skip && !glfwWindowShouldClose(window)) {
        double t = re_time_seconds() - t0;
        if (t > INTRO_LEN) break;
        glfwPollEvents();
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        if (!fbw || !fbh) continue;
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        igNewFrame();
        ImGuiIO* io = igGetIO_Nil();
        if (t > 0.3 && (igIsMouseClicked_Bool(ImGuiMouseButton_Left, false) || igIsKeyPressed_Bool(ImGuiKey_Escape, false) ||
                        igIsKeyPressed_Bool(ImGuiKey_Space, false) || igIsKeyPressed_Bool(ImGuiKey_Enter, false)))
            skip = true;
        intro_draw(t, io->DisplaySize.x, io->DisplaySize.y);
        igRender();
        end_platform_frame();
        ImDrawData* dd = igGetDrawData();
        for (int i = 0; dd && dd->Textures && i < dd->Textures->Size; i++) // (the font's glyphs: uploaded with the GPU idle)
            if (dd->Textures->Data[i]->Status != ImTextureStatus_OK) {
                vkDeviceWaitIdle(vk->device);
                ImGui_ImplVulkan_UpdateTexture(dd->Textures->Data[i]);
            }
        VkCommandBuffer cmd = vkctx_begin_frame(vk);
        if (!cmd) continue;
        vkctx_wait_submitted(vk);
        vkctx_begin_rendering(vk);
        ImGui_ImplVulkan_RenderDrawData(dd, cmd, VK_NULL_HANDLE);
        vkctx_end_frame(vk);
    }
}

// --- The loading screen: the picture slowly drifting closer, a light sweeping the logo now and then, dust, the
// engine's lines (translated), the status and the bar (eased toward the real progress, a glowing head) ---

void brand_splash_begin(void) {
    B.splash_t0 = re_time_seconds();
    B.shown = 0.0f;
    if (B.reader) B.video_t0 = B.splash_t0, B.frame_time = -1.0;
}

// The picture covering the screen (cropped, centered) with a slow zoom; maps the 1672 x 941 design's points
typedef struct Cover { float x, y, sx, sy; } Cover;
static Cover draw_cover(ImDrawList* dl, const BrandTex* t, float w, float h, float zoom, float alpha) {
    float ia = (float)t->w / (float)t->h, sa = w / h, dw = w, dh = h;
    if (ia > sa) dw = h * ia;
    else dh = w / ia;
    dw *= zoom, dh *= zoom;
    float x = (w - dw) * 0.5f, y = (h - dh) * 0.5f;
    ImDrawList_AddImage(dl, (ImTextureRef_c){ NULL, (ImTextureID)(uintptr_t)t->set }, (ImVec2_c){ x, y }, (ImVec2_c){ x + dw, y + dh },
                        (ImVec2_c){ 0, 0 }, (ImVec2_c){ 1, 1 }, rgba(1, 1, 1, alpha));
    Cover c = { x, y, dw / 1672.0f, dh / 941.0f };
    return c;
}
static ImVec2_c at(const Cover* c, float x, float y) { return (ImVec2_c){ c->x + x * c->sx, c->y + y * c->sy }; }

void brand_splash_draw(double t, float progress, const char* status) {
    ImGuiIO* io = igGetIO_Nil();
    float w = io->DisplaySize.x, h = io->DisplaySize.y, ft = (float)t;
    ImDrawList* dl = igGetBackgroundDrawList(NULL);
    ImDrawList_AddRectFilled(dl, (ImVec2_c){ 0, 0 }, (ImVec2_c){ w, h }, 0xFF000000u, 0.0f, 0); // theme: literal (the intro's black: the brand's, not a theme's)
    if (!B.ready || w < 1.0f || h < 1.0f) return; // (no window: Android took it, the screen off; the text's size would be 0)
    float fade = smooth(0.0f, 0.6f, ft);
    static double last;
    double now = re_time_seconds(), step = last > 0.0 ? now - last : 0.016;
    last = now;
    B.shown += (clamp01(progress) - B.shown) * clamp01((float)step * 7.0f); // eased (by time: the same at any frame rate)
    if (B.has_custom) { // the project's picture (or video): its own look, with the bar over it
        video_update();
        Cover c = draw_cover(dl, &B.custom, w, h, 1.0f + 0.02f * smooth(0.0f, 20.0f, ft), fade);
        (void)c;
    } else {
        Cover c = draw_cover(dl, &B.splash, w, h, 1.0f + 0.035f * smooth(0.0f, 25.0f, ft), fade);
        float sc = c.sy; // the design's pixel on screen
        // the light sweeping the logo every 4 s (a slanted bar crossing it)
        float ph = fmodf(ft, 4.0f) / 1.2f;
        if (ph < 1.0f) {
            float lx = 590.0f + ph * 520.0f, a = sinf(ph * 3.14159f) * 0.22f * fade;
            ImVec2_c p1 = at(&c, lx, 215), p2 = at(&c, lx + 40, 215), p3 = at(&c, lx - 60, 585), p4 = at(&c, lx - 100, 585);
            ImDrawList_AddQuadFilled(dl, p1, p2, p3, p4, rgba(1, 1, 1, a));
        }
        // the dust drifting across the light
        for (int i = 0; i < 70; i++) {
            float sx = hash1((float)i * 2.3f), sy = hash1((float)i * 5.7f), sp = 8.0f + hash1((float)i * 9.1f) * 22.0f;
            float x = fmodf(sx * 1672.0f + ft * sp, 1672.0f), y = sy * 941.0f + sinf(ft * 0.7f + (float)i) * 6.0f;
            float a = (0.15f + 0.35f * hash1((float)i * 4.1f)) * fade * (0.6f + 0.4f * sinf(ft * 2.0f + (float)i));
            ImDrawList_AddCircleFilled(dl, at(&c, x, y), (0.8f + hash1((float)i * 1.3f) * 1.6f) * sc, rgba(0.85f, 0.9f, 1.0f, a), 6);
        }
        ImU32 dim = rgba(0.72f, 0.75f, 0.8f, 0.85f * fade), faint = rgba(0.55f, 0.58f, 0.63f, 0.8f * fade);
        float fs = 13.0f * sc;
        // top left: the engine, its version, its build; top right: what it stands for
        ImVec2_c tl = at(&c, 74, 40);
        text_at(dl, tl.x, tl.y, fs, dim, "REAL ENGINE", 0);
        char v[64];
        snprintf(v, sizeof v, "v%s", RE_VERSION_STRING);
        text_at(dl, tl.x, tl.y + fs * 1.5f, fs, faint, v, 0);
        ImDrawList_AddLine(dl, at(&c, 46, 34), at(&c, 46, 102), faint, 1.0f);
        ImVec2_c trp = at(&c, 1601, 40);
        text_at(dl, trp.x, trp.y, fs, faint, TR("HIGH PERFORMANCE."), 2);
        text_at(dl, trp.x, trp.y + fs * 1.5f, fs, faint, TR("NEXT GEN GRAPHICS."), 2);
        text_at(dl, trp.x, trp.y + fs * 3.0f, fs, faint, TR("INFINITE POSSIBILITIES."), 2);
        ImDrawList_AddLine(dl, at(&c, 1626, 34), at(&c, 1626, 102), faint, 1.0f);
        // under the name
        char line[160];
        snprintf(line, sizeof line, "%s   \xe2\x80\xa2   %s   \xe2\x80\xa2   %s", TR("CREATE"), TR("RENDER"), TR("PLAY"));
        ImVec2_c un = at(&c, 836, 603);
        text_spaced(dl, un.x, un.y, 18.0f * sc, 6.0f * sc, rgba(0.68f, 0.71f, 0.76f, fade), line);
        // the features under their icons (each a two-line phrase: the word order is the language's)
        static const char* const feats[4] = { "OPTIMIZED\nPERFORMANCE", "DYNAMIC\nLIGHTING", "MASSIVE\nSCENES", "REALISTIC\n& STYLIZED" };
        static const float fx[4] = { 91, 224, 344, 462 };
        for (int k = 0; k < 4; k++) {
            ImVec2_c p = at(&c, fx[k], 868);
            char two[96];
            snprintf(two, sizeof two, "%s", TR(feats[k]));
            char* nl = strchr(two, '\n');
            if (nl) *nl = 0;
            text_at(dl, p.x, p.y, 11.5f * sc, faint, two, 1);
            if (nl) text_at(dl, p.x, p.y + 15.0f * sc, 11.5f * sc, faint, nl + 1, 1);
        }
        ImVec2_c br = at(&c, 1628, 870);
        text_at(dl, br.x, br.y, 13.0f * sc, dim, "REAL ENGINE", 2);
        text_at(dl, br.x, br.y + 18.0f * sc, 10.5f * sc, faint, TR("\xc2\xa9 2026 Real Engine. All rights reserved."), 2);
    }
    // the status and the bar (the same place on any picture: the lower middle)
    float bw = w * 0.47f, bx = (w - bw) * 0.5f, by = h * 0.80f, bh = fmaxf(h * 0.006f, 4.0f), fsz = fmaxf(h * 0.017f, 13.0f);
    ImDrawList_AddRectFilled(dl, (ImVec2_c){ bx - fsz, by - fsz * 2.6f }, (ImVec2_c){ bx + bw + fsz * 4.0f, by + bh + fsz }, rgba(0, 0, 0, 0.35f * fade),
                             fsz * 0.4f, 0);
    text_spaced(dl, w * 0.5f, by - fsz * 1.8f, fsz, fsz * 0.12f, rgba(0.86f, 0.88f, 0.92f, fade), status ? status : "");
    ImDrawList_AddRectFilled(dl, (ImVec2_c){ bx, by }, (ImVec2_c){ bx + bw, by + bh }, rgba(0.25f, 0.27f, 0.3f, 0.8f * fade), 0.0f, 0);
    float fx2 = bx + bw * B.shown;
    // (2026-11-23) in the R's blue (Azul Real's accent), deeper at its start, lighter at its head
    ImDrawList_AddRectFilledMultiColor(dl, (ImVec2_c){ bx, by }, (ImVec2_c){ fx2, by + bh }, rgba(0.10f, 0.29f, 0.70f, fade), rgba(0.30f, 0.55f, 1.0f, fade),
                                       rgba(0.30f, 0.55f, 1.0f, fade), rgba(0.10f, 0.29f, 0.70f, fade));
    for (int r = 1; r <= 4; r++) // the head's glow
        ImDrawList_AddCircleFilled(dl, (ImVec2_c){ fx2, by + bh * 0.5f }, bh * (0.8f + r * 1.1f), rgba(0.25f, 0.5f, 1.0f, 0.09f * fade), 20);
    float shimmer = fmodf(ft * 0.6f, 1.0f); // a light running along the filled part
    if (B.shown > 0.02f) {
        float sx = bx + (fx2 - bx) * shimmer;
        ImDrawList_AddRectFilledMultiColor(dl, (ImVec2_c){ fmaxf(bx, sx - bw * 0.06f), by }, (ImVec2_c){ fminf(fx2, sx + bw * 0.06f), by + bh },
                                           rgba(0.6f, 0.8f, 1, 0), rgba(0.7f, 0.85f, 1, 0.8f * fade), rgba(0.7f, 0.85f, 1, 0.8f * fade), rgba(0.6f, 0.8f, 1, 0));
    }
    char pct[16];
    snprintf(pct, sizeof pct, "%d%%", (int)(B.shown * 100.0f + 0.5f));
    text_at(dl, bx + bw + fsz * 0.8f, by - fsz * 0.45f, fsz, rgba(0.86f, 0.88f, 0.92f, fade), pct, 0);
}

void brand_splash_frame(VkCtx* vk, float progress, const char* status) {
    crash_heartbeat(); // (each step of a load: progress, not a hang)
    if (!g_brand_splash_on || !B.ready) return;
    glfwPollEvents();
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    igNewFrame();
    brand_splash_draw(re_time_seconds() - B.splash_t0, progress, status);
    igRender();
    end_platform_frame();
    ImDrawData* dd = igGetDrawData();
    for (int i = 0; dd && dd->Textures && i < dd->Textures->Size; i++)
        if (dd->Textures->Data[i]->Status != ImTextureStatus_OK) {
            vkDeviceWaitIdle(vk->device);
            ImGui_ImplVulkan_UpdateTexture(dd->Textures->Data[i]);
        }
    VkCommandBuffer cmd = vkctx_begin_frame(vk);
    if (!cmd) return;
    vkctx_wait_submitted(vk);
    vkctx_begin_rendering(vk);
    ImGui_ImplVulkan_RenderDrawData(dd, cmd, VK_NULL_HANDLE);
    vkctx_end_frame(vk);
}

static double s_fade_t0;
void brand_splash_end(VkCtx* vk) {
    if (!g_brand_splash_on || !B.ready) return;
    // shown for BRAND_SPLASH_MIN_S at least (a quick load doesn't flash it): the bar runs on to the end meanwhile
    double t0 = re_time_seconds(), left = BRAND_SPLASH_MIN_S - (t0 - B.splash_t0);
    float from = B.shown;
    for (double t = t0; t - t0 < left; t = re_time_seconds())
        brand_splash_frame(vk, from + (1.0f - from) * (float)((t - t0) / left), "");
    for (int i = 0; i < 6; i++) brand_splash_frame(vk, 1.0f, ""); // (the bar reaches the end)
    s_fade_t0 = -1.0; // (then it fades over the editor: brand_splash_fade, timed from the editor's first frame)
}

// The loading screen fading out over the editor's first frames (it used to vanish at once, the editor still settling)
void brand_splash_fade(void) {
    if (s_fade_t0 == 0.0 || !B.ready) return;
    if (s_fade_t0 < 0.0) s_fade_t0 = re_time_seconds();
    float a = 1.0f - (float)((re_time_seconds() - s_fade_t0) / 0.45);
    if (a <= 0.0f) {
        s_fade_t0 = 0.0;
        return;
    }
    a = a * a * (3.0f - 2.0f * a);
    ImGuiIO* io = igGetIO_Nil();
    ImDrawList* dl = igGetForegroundDrawList_ViewportPtr(NULL);
    ImDrawList_AddRectFilled(dl, (ImVec2_c){ 0, 0 }, (ImVec2_c){ io->DisplaySize.x, io->DisplaySize.y }, rgba(0, 0, 0, a), 0.0f, 0);
    draw_cover(dl, B.has_custom ? &B.custom : &B.splash, io->DisplaySize.x, io->DisplaySize.y, 1.0f, a);
}
