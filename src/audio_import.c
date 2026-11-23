// Importing sounds (the editor; notas-dev/audio.md): a file becomes a Sound Wave (.rsound) next to where it was dropped.
// WAV by our own reader (PCM 8/16/24/32 bits, float 32/64, WAVE_FORMAT_EXTENSIBLE, any rate); MP3, AAC/M4A, WMA,
// FLAC, OGG/Opus (where Windows has their codec) through Media Foundation's source reader. More than two channels:
// folded to stereo. Long sounds (> 15 s) stream (paged in while they play) and go to the Music class.
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "audio_internal.h"
#include "platform.h"

typedef struct Pcm { int16_t* s; uint32_t frames, channels, rate, cap; } Pcm;

static bool pcm_grow(Pcm* p, uint32_t frames) {
    if (p->frames + frames <= p->cap) return true;
    uint32_t cap = p->cap ? p->cap : 65536;
    while (cap < p->frames + frames) cap *= 2;
    int16_t* n = realloc(p->s, (size_t)cap * p->channels * sizeof(int16_t)); // (an import, not a frame)
    if (!n) return false;
    p->s = n, p->cap = cap;
    return true;
}

static int16_t to16(double v) {
    v *= 32767.0;
    return (int16_t)(v > 32767.0 ? 32767 : v < -32768.0 ? -32768 : lrint(v));
}

// A sample of format (bits, float) at p
static double sample_at(const uint8_t* p, uint32_t bits, bool flt) {
    if (flt) {
        if (bits == 64) { double d; memcpy(&d, p, 8); return d; }
        float f; memcpy(&f, p, 4); return f;
    }
    switch (bits) {
    case 8: return ((int)p[0] - 128) / 128.0;
    case 16: return (int16_t)(p[0] | p[1] << 8) / 32768.0;
    case 24: { int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24); return (v >> 8) / 8388608.0; }
    default: { int32_t v; memcpy(&v, p, 4); return v / 2147483648.0; }
    }
}

// n frames of ch channels into stereo (or mono) int16: more than two folded (Dolby's: center and surrounds at -3 dB)
static void fold_in(Pcm* out, const uint8_t* data, uint32_t n, uint32_t ch, uint32_t bits, bool flt) {
    uint32_t bpf = ch * (bits / 8);
    if (!pcm_grow(out, n)) return;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* f = data + (size_t)i * bpf;
        double s[8] = { 0 };
        for (uint32_t c = 0; c < ch && c < 8; c++) s[c] = sample_at(f + c * (bits / 8), bits, flt);
        int16_t* o = out->s + (size_t)(out->frames + i) * out->channels;
        if (out->channels == 1) o[0] = to16(s[0]);
        else if (ch <= 2) o[0] = to16(s[0]), o[1] = to16(ch == 2 ? s[1] : s[0]);
        else { // L R C LFE Ls Rs (Lb Rb)
            double l = s[0] + 0.7071 * s[2] + 0.7071 * s[4] + (ch > 6 ? 0.7071 * s[6] : 0.0);
            double r = s[1] + 0.7071 * s[2] + 0.7071 * s[5] + (ch > 7 ? 0.7071 * s[7] : 0.0);
            o[0] = to16(l * 0.5), o[1] = to16(r * 0.5);
        }
    }
    out->frames += n;
}

static bool read_wav(const char* path, Pcm* out) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = size > 44 ? malloc((size_t)size) : NULL;
    bool ok = b && fread(b, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) {
        free(b);
        return false;
    }
    uint32_t fmt = 0, ch = 0, rate = 0, bits = 0;
    bool flt = false, have_fmt = false;
    for (size_t p = 12; p + 8 <= (size_t)size;) {
        uint32_t len;
        memcpy(&len, b + p + 4, 4);
        const uint8_t* d = b + p + 8;
        if (p + 8 + len > (size_t)size) len = (uint32_t)(size - p - 8);
        if (!memcmp(b + p, "fmt ", 4) && len >= 16) {
            fmt = d[0] | d[1] << 8, ch = d[2] | d[3] << 8;
            memcpy(&rate, d + 4, 4);
            bits = d[14] | d[15] << 8;
            if (fmt == 0xFFFE && len >= 40) fmt = d[24] | d[25] << 8; // (EXTENSIBLE: the sub-format's tag)
            flt = fmt == 3;
            have_fmt = (fmt == 1 || fmt == 3) && ch >= 1 && ch <= 8 && (bits == 8 || bits == 16 || bits == 24 || bits == 32 || bits == 64);
        } else if (!memcmp(b + p, "data", 4) && have_fmt) {
            out->channels = ch >= 2 ? 2 : 1, out->rate = rate;
            fold_in(out, d, len / (ch * (bits / 8)), ch, bits, flt);
            free(b);
            return out->frames > 0;
        }
        p += 8 + len + (len & 1);
    }
    free(b);
    return false;
}

static bool read_mf(const char* path, Pcm* out) {
    wchar_t wp[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024)) return false;
    bool com = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        if (com) CoUninitialize();
        return false;
    }
    IMFSourceReader* rd = NULL;
    bool ok = SUCCEEDED(MFCreateSourceReaderFromURL(wp, NULL, &rd));
    IMFMediaType* mt = NULL;
    if (ok) { // decoded to 16-bit PCM (the reader picks the decoder)
        MFCreateMediaType(&mt);
        IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
        IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
        IMFMediaType_SetUINT32(mt, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        ok = SUCCEEDED(IMFSourceReader_SetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, mt));
        IMFMediaType_Release(mt);
    }
    IMFMediaType* cur = NULL;
    UINT32 ch = 0, rate = 0, bits = 0;
    if (ok && SUCCEEDED(IMFSourceReader_GetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur))) {
        IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_NUM_CHANNELS, &ch);
        IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        IMFMediaType_Release(cur);
    } else ok = false;
    ok = ok && ch >= 1 && ch <= 8 && rate >= 1000 && bits == 16;
    if (ok) out->channels = ch >= 2 ? 2 : 1, out->rate = rate;
    while (ok) {
        DWORD flags = 0;
        IMFSample* smp = NULL;
        if (FAILED(IMFSourceReader_ReadSample(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, NULL, &smp))) break;
        if (smp) {
            IMFMediaBuffer* buf = NULL;
            if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(smp, &buf))) {
                BYTE* d = NULL;
                DWORD len = 0;
                if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &d, NULL, &len))) {
                    fold_in(out, d, len / (ch * 2), ch, 16, false);
                    IMFMediaBuffer_Unlock(buf);
                }
                IMFMediaBuffer_Release(buf);
            }
            IMFSample_Release(smp);
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    if (rd) IMFSourceReader_Release(rd);
    MFShutdown();
    if (com) CoUninitialize();
    return ok && out->frames > 0;
}

// The content importer (content_set_importer): src -> <folder>.rsound (folder: "<dir>/<the file's name>")
bool audio_import_cb(const char* src, const char* folder_vpath, bool place, char* msg, size_t cap);
bool audio_import_cb(const char* src, const char* folder_vpath, bool place, char* msg, size_t cap) {
    (void)place;
    Pcm p;
    memset(&p, 0, sizeof p);
    const char* ext = strrchr(src, '.');
    bool ok = ext && !_stricmp(ext, ".wav") ? read_wav(src, &p) : false;
    if (!ok) { // (a WAV in a codec we don't read: Media Foundation's turn too)
        free(p.s);
        memset(&p, 0, sizeof p);
        ok = read_mf(src, &p);
    }
    if (!ok) {
        free(p.s);
        snprintf(msg, cap, "%s", ext && (!_stricmp(ext, ".ogg") || !_stricmp(ext, ".opus"))
                                    ? "Could not decode the sound (OGG needs Windows' Web Media Extensions)."
                                    : "Could not decode the sound.");
        return false;
    }
    char vp[700], disk[1200];
    snprintf(vp, sizeof vp, "%s.rsound", folder_vpath);
    const char* root = audio_content_root();
    if (!strncmp(vp, "/Content", 8)) snprintf(disk, sizeof disk, "%s%s", root, vp + 8);
    else snprintf(disk, sizeof disk, "%s", vp);
    double secs = (double)p.frames / p.rate;
    bool stream = secs > 15.0;
    ok = audio_wave_write(disk, p.s, p.frames, p.channels, p.rate, stream);
    free(p.s);
    if (!ok) {
        snprintf(msg, cap, "Could not write %s.", vp);
        return false;
    }
    audio_rescan();
    SoundWave* w = audio_wave_load(vp);
    if (w && stream) { // long: music
        w->h.sound_class = au_builtin_class(AU_SC_MUSIC);
        audio_wave_save(w);
    }
    snprintf(msg, cap, "Imported %s (%.1f s, %u Hz, %s%s).", vp, secs, p.rate, p.channels == 2 ? "stereo" : "mono", stream ? ", streamed" : "");
    return true;
}
