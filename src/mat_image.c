// Image files for the materials' Image Texture nodes, through the Windows Imaging Component (apart from material.c:
// the Windows headers and the node constants clash)
#define COBJMACROS
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#else // Android: the NDK's image decoder (PNG, JPEG, WebP, GIF, HEIF...; API 30)
#include <android/imagedecoder.h>
#endif

#include "platform.h"

static char* read_all(const char* path, size_t* len) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) free(b), b = NULL;
    fclose(f);
    if (b) *len = (size_t)n;
    return b;
}

#ifdef _WIN32
static const GUID MAT_CLSID_WICImagingFactory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID MAT_IID_IWICImagingFactory = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID MAT_GUID_WICPixelFormat32bppRGBA = { 0xf5c7ad2d, 0x6a8d, 0x43dd, { 0xa7, 0xa8, 0xa2, 0x99, 0x35, 0x26, 0x1a, 0xe9 } };
#endif

// --- Targa (Windows has no WIC codec for it: game textures often are .tga; the Tlender's image_read.c, adapted) ---
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static void tga_px(const uint8_t* p, int bpp, const uint8_t* pal, int pal_bits, int pal_n, int pal_first, bool gray, uint8_t o[4]) {
    if (pal) {
        int i = (bpp == 16 ? le16(p) : p[0]) - pal_first;
        if (i < 0 || i >= pal_n) i = 0;
        tga_px(pal + (size_t)i * ((pal_bits + 7) / 8), pal_bits == 15 ? 16 : pal_bits, NULL, 0, 0, 0, false, o);
        return;
    }
    if (gray) {
        o[0] = o[1] = o[2] = p[0];
        o[3] = bpp == 16 ? p[1] : 255;
        return;
    }
    if (bpp == 15 || bpp == 16) {
        uint16_t v = le16(p);
        o[2] = (uint8_t)(((v & 31) * 255 + 15) / 31);
        o[1] = (uint8_t)((((v >> 5) & 31) * 255 + 15) / 31);
        o[0] = (uint8_t)((((v >> 10) & 31) * 255 + 15) / 31);
        o[3] = (v & 0x8000) ? 255 : 0;
        return;
    }
    o[0] = p[2], o[1] = p[1], o[2] = p[0];
    o[3] = bpp == 32 ? p[3] : 255;
}
static uint8_t* load_tga(const uint8_t* d, size_t n, uint32_t* ow, uint32_t* oh) {
    if (n < 18) return NULL;
    int idlen = d[0], cmtype = d[1], type = d[2];
    int pal_first = le16(d + 3), pal_n = le16(d + 5), pal_bits = d[7];
    int w = le16(d + 12), h = le16(d + 14), bpp = d[16], desc = d[17];
    bool rle = type >= 9, gray = (type & 7) == 3, mapped = (type & 7) == 1;
    int bytes = (bpp + 7) / 8;
    if (!((type & 7) >= 1 && (type & 7) <= 3) || (mapped && !cmtype) || bytes < 1 || bytes > 4 || w <= 0 || h <= 0 || w > 16384 || h > 16384)
        return NULL;
    size_t p = 18 + (size_t)idlen;
    const uint8_t* pal = NULL;
    if (cmtype) {
        size_t pl = (size_t)pal_n * ((pal_bits + 7) / 8);
        if (p + pl > n) return NULL;
        pal = d + p;
        p += pl;
    }
    if (!mapped) pal = NULL;
    size_t total = (size_t)w * h;
    uint8_t* out = malloc(total * 4);
    if (!out) return NULL;
    bool alpha_bits = (desc & 15) > 0 || (pal && pal_bits == 32), any_alpha = false;
    uint8_t px[4] = { 0 };
    for (size_t i = 0; i < total;) {
        int run = 1;
        bool repeat = false;
        if (rle) {
            if (p >= n) goto bad;
            run = (d[p] & 127) + 1, repeat = (d[p] & 128) != 0;
            p++;
        }
        for (int k = 0; k < run && i < total; k++, i++) {
            if (!repeat || k == 0) {
                if (p + (size_t)bytes > n) goto bad;
                tga_px(d + p, bpp, pal, pal_bits, pal_n, pal_first, gray, px);
                p += (size_t)bytes;
            }
            int x = (int)(i % (size_t)w), y = (int)(i / (size_t)w);
            if (desc & 0x10) x = w - 1 - x;
            if (!(desc & 0x20)) y = h - 1 - y; // bottom-up unless the top-left origin bit
            memcpy(out + ((size_t)y * w + x) * 4, px, 4);
            any_alpha |= px[3] != 0;
        }
    }
    // no alpha declared, or an alpha channel all zero (writers that leave it out): opaque
    bool has_alpha_ch = (bpp == 32 || bpp == 16 || (pal && (pal_bits == 32 || pal_bits == 16))) && !(gray && bpp == 8);
    if (has_alpha_ch && (!alpha_bits || !any_alpha) && !(gray && bpp == 16))
        for (size_t i = 0; i < total; i++) out[i * 4 + 3] = 255;
    *ow = (uint32_t)w, *oh = (uint32_t)h;
    return out;
bad:
    free(out);
    return NULL;
}

static bool ext_tga(const char* path) {
    const char* dot = strrchr(path, '.');
    return dot && (!_stricmp(dot, ".tga") || !_stricmp(dot, ".tpic"));
}

// (Q23) A Radiance .hdr (RGBE: the HDRIs of Poly Haven and the skies' files) as linear floats, RGB per pixel; the
// header's "-Y h +X w" only (every writer's), the scanlines flat or run-length encoded (the "new" RLE: 2 2 hi lo, then
// each channel's runs). WIC has no decoder for it: before this the Content Browser listed .hdr files it couldn't open.
// malloc'd, NULL when it isn't one.
float* mat_hdr_decode(const uint8_t* d, size_t n, uint32_t* ow, uint32_t* oh) {
    if (n < 11 || (memcmp(d, "#?RADIANCE", 10) != 0 && memcmp(d, "#?RGBE", 6) != 0)) return NULL;
    size_t p = 0;
    bool rgbe = false;
    for (;;) { // the header's lines up to the empty one
        size_t e = p;
        while (e < n && d[e] != '\n') e++;
        if (e >= n) return NULL;
        if (e == p) break;
        if (e - p >= 23 && !memcmp(d + p, "FORMAT=32-bit_rle_rgbe", 22)) rgbe = true;
        p = e + 1;
    }
    p++;
    char res[64] = { 0 };
    size_t e = p;
    while (e < n && d[e] != '\n' && e - p < sizeof res - 1) e++;
    if (e >= n) return NULL;
    memcpy(res, d + p, e - p);
    p = e + 1;
    int w = 0, h = 0;
    (void)rgbe; // (some writers leave FORMAT out: the pixels are RGBE all the same)
    if (sscanf(res, "-Y %d +X %d", &h, &w) != 2 || w <= 0 || h <= 0 || w > 16384 || h > 16384) return NULL;
    float* out = malloc((size_t)w * h * 3 * sizeof(float));
    uint8_t* line = malloc((size_t)w * 4);
    if (!out || !line) goto bad;
    for (int y = 0; y < h; y++) {
        if (w >= 8 && w < 32768 && p + 4 <= n && d[p] == 2 && d[p + 1] == 2 && !(d[p + 2] & 0x80)) { // the new RLE
            if (((int)d[p + 2] << 8 | d[p + 3]) != w) goto bad;
            p += 4;
            for (int c = 0; c < 4; c++)
                for (int x = 0; x < w;) {
                    if (p >= n) goto bad;
                    int run = d[p++];
                    if (run > 128) { // a run of one value
                        run -= 128;
                        if (p >= n || x + run > w) goto bad;
                        for (int k = 0; k < run; k++) line[(x + k) * 4 + c] = d[p];
                        p++;
                    } else { // literals
                        if (!run || x + run > w || p + (size_t)run > n) goto bad;
                        for (int k = 0; k < run; k++) line[(x + k) * 4 + c] = d[p + k];
                        p += (size_t)run;
                    }
                    x += run;
                }
        } else { // flat (or the old RLE's repeats: 1 1 1 n repeats the last pixel)
            for (int x = 0; x < w; x++) {
                if (p + 4 > n) goto bad;
                if (d[p] == 1 && d[p + 1] == 1 && d[p + 2] == 1 && x > 0) {
                    int run = d[p + 3];
                    for (int k = 0; k < run && x < w; k++, x++) memcpy(line + x * 4, line + (x - 1) * 4, 4);
                    x--;
                } else
                    memcpy(line + x * 4, d + p, 4);
                p += 4;
            }
        }
        float* o = out + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            const uint8_t* q = line + x * 4;
            float f = q[3] ? ldexpf(1.0f, (int)q[3] - (128 + 8)) : 0.0f;
            o[x * 3 + 0] = (q[0] + 0.5f) * f, o[x * 3 + 1] = (q[1] + 0.5f) * f, o[x * 3 + 2] = (q[2] + 0.5f) * f;
        }
    }
    free(line);
    *ow = (uint32_t)w, *oh = (uint32_t)h;
    return out;
bad:
    free(out), free(line);
    return NULL;
}
static uint8_t srgb8(float v) { // linear to sRGB, clamped (the display's exposure 0: what a texture sampling it gets)
    v = v <= 0.0f ? 0.0f : v >= 1.0f ? 1.0f : v;
    v = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
    return (uint8_t)(v * 255.0f + 0.5f);
}
// The .hdr as 8-bit sRGBA at an exposure (stops; 0: as is): the texture's and the viewer's pixels
uint8_t* mat_hdr_rgba8(const float* rgb, uint32_t w, uint32_t h, float exposure) {
    uint8_t* px = malloc((size_t)w * h * 4);
    if (!px) return NULL;
    float k = exp2f(exposure);
    for (size_t i = 0; i < (size_t)w * h; i++)
        px[i * 4 + 0] = srgb8(rgb[i * 3 + 0] * k), px[i * 4 + 1] = srgb8(rgb[i * 3 + 1] * k), px[i * 4 + 2] = srgb8(rgb[i * 3 + 2] * k), px[i * 4 + 3] = 255;
    return px;
}
static bool ext_is(const char* path, const char* ext) {
    const char* dot = strrchr(path, '.');
    return dot && !_stricmp(dot, ext);
}

// An image file as 8-bit RGBA through the Windows Imaging Component (PNG, JPEG, BMP, TIFF, GIF...; copied and adapted
// from the Tlender's image_wic.c), Targa and Radiance .hdr (Q23) decoded here. malloc'd pixels, NULL when it can't be read.
uint8_t* mat_load_image_rgba_mem(const void* data, size_t n, uint32_t* w, uint32_t* h);
uint8_t* udim_preview_rgba(const char* path, uint32_t* w, uint32_t* h); // (Q18) udim.c
uint8_t* mat_load_image_rgba(const char* path, uint32_t* w, uint32_t* h) {
    size_t pl = strlen(path);
    if (pl > 6 && !_stricmp(path + pl - 6, ".rudim")) return udim_preview_rgba(path, w, h); // (Q18) its first tile
    size_t n;
    char* data = read_all(path, &n);
    if (!data) return NULL;
    if (ext_is(path, ".hdr")) { // (Q23)
        float* rgb = mat_hdr_decode((const uint8_t*)data, n, w, h);
        free(data);
        uint8_t* px = rgb ? mat_hdr_rgba8(rgb, *w, *h, 0.0f) : NULL;
        free(rgb);
        return px;
    }
    uint8_t* px = ext_tga(path) ? load_tga((const uint8_t*)data, n, w, h) : NULL;
    if (!px) px = mat_load_image_rgba_mem(data, n, w, h);
    free(data);
    return px;
}

#ifdef _WIN32
// The same from a file already in memory (the images embedded in the executable: brand.c)
uint8_t* mat_load_image_rgba_mem(const void* data, size_t n, uint32_t* w, uint32_t* h) {
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool uninit = SUCCEEDED(co);
    IWICImagingFactory* f = NULL;
    IWICStream* st = NULL;
    IWICBitmapDecoder* dec = NULL;
    IWICBitmapFrameDecode* fr = NULL;
    IWICFormatConverter* cv = NULL;
    uint8_t* px = NULL;
    UINT iw = 0, ih = 0;
    if (FAILED(CoCreateInstance(&MAT_CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &MAT_IID_IWICImagingFactory, (void**)&f))) goto done;
    if (FAILED(IWICImagingFactory_CreateStream(f, &st)) || FAILED(IWICStream_InitializeFromMemory(st, (BYTE*)data, (DWORD)n))) goto done;
    if (FAILED(IWICImagingFactory_CreateDecoderFromStream(f, (IStream*)st, NULL, WICDecodeMetadataCacheOnDemand, &dec))) goto done;
    if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) || FAILED(IWICBitmapFrameDecode_GetSize(fr, &iw, &ih))) goto done;
    if (iw == 0 || ih == 0 || iw > 16384 || ih > 16384) goto done;
    if (FAILED(IWICImagingFactory_CreateFormatConverter(f, &cv)) ||
        FAILED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource*)fr, &MAT_GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0.0,
                                              WICBitmapPaletteTypeCustom)))
        goto done;
    px = malloc((size_t)iw * ih * 4);
    if (px && FAILED(IWICFormatConverter_CopyPixels(cv, NULL, iw * 4, iw * ih * 4, px))) free(px), px = NULL;
done:
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (st) IWICStream_Release(st);
    if (f) IWICImagingFactory_Release(f);
    if (uninit) CoUninitialize();
    *w = iw, *h = ih;
    return px;
}


// A heightmap's samples (landscape.c): the image as 16-bit gray through the Windows Imaging Component (a 16-bit PNG or
// TIFF read exactly, 8 bits or color by their luminance). malloc'd, NULL when it can't be read.
static const GUID MAT_GUID_WICPixelFormat16bppGray = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0b } };
uint16_t* mat_load_image_gray16(const char* path, uint32_t* w, uint32_t* h) {
    size_t n;
    char* data = read_all(path, &n);
    if (!data) return NULL;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool uninit = SUCCEEDED(co);
    IWICImagingFactory* f = NULL;
    IWICStream* st = NULL;
    IWICBitmapDecoder* dec = NULL;
    IWICBitmapFrameDecode* fr = NULL;
    IWICFormatConverter* cv = NULL;
    uint16_t* px = NULL;
    UINT iw = 0, ih = 0;
    if (FAILED(CoCreateInstance(&MAT_CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &MAT_IID_IWICImagingFactory, (void**)&f))) goto done;
    if (FAILED(IWICImagingFactory_CreateStream(f, &st)) || FAILED(IWICStream_InitializeFromMemory(st, (BYTE*)data, (DWORD)n))) goto done;
    if (FAILED(IWICImagingFactory_CreateDecoderFromStream(f, (IStream*)st, NULL, WICDecodeMetadataCacheOnDemand, &dec))) goto done;
    if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) || FAILED(IWICBitmapFrameDecode_GetSize(fr, &iw, &ih))) goto done;
    if (iw < 2 || ih < 2 || iw > 16384 || ih > 16384) goto done;
    if (FAILED(IWICImagingFactory_CreateFormatConverter(f, &cv)) ||
        FAILED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource*)fr, &MAT_GUID_WICPixelFormat16bppGray, WICBitmapDitherTypeNone, NULL, 0.0,
                                              WICBitmapPaletteTypeCustom)))
        goto done;
    px = malloc((size_t)iw * ih * 2);
    if (px && FAILED(IWICFormatConverter_CopyPixels(cv, NULL, iw * 2, iw * ih * 2, (BYTE*)px))) free(px), px = NULL;
done:
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (st) IWICStream_Release(st);
    if (f) IWICImagingFactory_Release(f);
    if (uninit) CoUninitialize();
    free(data);
    *w = iw, *h = ih;
    return px;
}
#else
// Android: AImageDecoder into RGBA8 (premultiplied off: the materials blend straight alpha)
uint8_t* mat_load_image_rgba_mem(const void* data, size_t n, uint32_t* w, uint32_t* h) {
    *w = *h = 0;
    AImageDecoder* d = NULL;
    if (AImageDecoder_createFromBuffer(data, n, &d) != ANDROID_IMAGE_DECODER_SUCCESS || !d) return NULL;
    AImageDecoder_setAndroidBitmapFormat(d, ANDROID_BITMAP_FORMAT_RGBA_8888);
    AImageDecoder_setUnpremultipliedRequired(d, true);
    const AImageDecoderHeaderInfo* hi = AImageDecoder_getHeaderInfo(d);
    int32_t iw = AImageDecoderHeaderInfo_getWidth(hi), ih = AImageDecoderHeaderInfo_getHeight(hi);
    uint8_t* px = NULL;
    if (iw > 0 && ih > 0 && iw <= 16384 && ih <= 16384) {
        size_t stride = AImageDecoder_getMinimumStride(d);
        px = malloc(stride * (size_t)ih); // (a decoded image: the caller frees it)
        if (px && AImageDecoder_decodeImage(d, px, stride, stride * (size_t)ih) != ANDROID_IMAGE_DECODER_SUCCESS) free(px), px = NULL;
        if (px && stride != (size_t)iw * 4) // (rows packed)
            for (int32_t y = 1; y < ih; y++) memmove(px + (size_t)y * iw * 4, px + (size_t)y * stride, (size_t)iw * 4);
    }
    AImageDecoder_delete(d);
    if (px) *w = (uint32_t)iw, *h = (uint32_t)ih;
    return px;
}
// (a heightmap's 16-bit samples: imported in the editor; the packaged game reads the landscapes' own heights)
uint16_t* mat_load_image_gray16(const char* path, uint32_t* w, uint32_t* h) {
    (void)path;
    *w = *h = 0;
    return NULL;
}
#endif
