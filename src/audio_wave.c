// Sound Waves (.rsound, audio.h): written by the importer, mapped from their files (the OS pages them in), found by
// the hash of their vpath like every asset.
#include "audio.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "platform_files.h"

#define MAX_WAVES 4096

static struct {
    char root[512];
    SoundWave* waves[MAX_WAVES];
    uint32_t n;
    bool scanned;
} W;

uint32_t audio_vpath_hash(const char* vpath) { // (the Blueprints' bp_vpath_hash: FNV-1a of the lower-case vpath)
    uint32_t h = 2166136261u;
    for (const char* c = vpath; *c; c++) h = (h ^ (uint8_t)tolower((unsigned char)*c)) * 16777619u;
    return h ? h : 1u;
}

const char* audio_content_root(void) { return W.root; }

void audio_waves_root(const char* root); // (audio.c: audio_init, audio_set_content_root)
void audio_forget_wave(const SoundWave* s); // (audio.c: its voices stopped and the mixer done with it)
void audio_waves_root(const char* root) {
    snprintf(W.root, sizeof W.root, "%s", root ? root : "");
    for (size_t i = 0; W.root[i]; i++)
        if (W.root[i] == '\\') W.root[i] = '/';
    size_t l = strlen(W.root);
    if (l && W.root[l - 1] == '/') W.root[l - 1] = 0;
    W.scanned = false;
}

static void disk_of(const char* vpath, char* out, size_t cap) {
    if (!strncmp(vpath, "/Content", 8)) snprintf(out, cap, "%s%s", W.root, vpath + 8);
    else snprintf(out, cap, "%s", vpath);
}
static void vpath_of(const char* disk, char* out, size_t cap) {
    size_t rl = strlen(W.root);
    if (rl && !strncmp(disk, W.root, rl)) snprintf(out, cap, "/Content%s", disk + rl);
    else snprintf(out, cap, "%s", disk);
}

// The samples' outline, peak and loudness (the header's measures)
static void measure(SoundWaveHeader* h, const int16_t* pcm) {
    uint64_t n = (uint64_t)h->frames * h->channels;
    double sum = 0.0;
    int peak = 0;
    memset(h->wave, 0, sizeof h->wave);
    for (uint64_t i = 0; i < n; i++) {
        int v = pcm[i] < 0 ? -(int)pcm[i] : pcm[i];
        if (v > peak) peak = v;
        sum += (double)pcm[i] * pcm[i];
        uint64_t pt = (i / h->channels) * SND_WAVE_POINTS / (h->frames ? h->frames : 1);
        int q = v >> 7;
        if (pt < SND_WAVE_POINTS && q > h->wave[pt]) h->wave[pt] = (uint8_t)(q > 255 ? 255 : q);
    }
    h->peak = (float)peak / 32768.0f;
    h->rms = n ? (float)(sqrt(sum / (double)n) / 32768.0) : 0.0f;
}

bool audio_wave_write(const char* path, const int16_t* pcm, uint32_t frames, uint32_t channels, uint32_t rate, bool stream) {
    if (!pcm || !frames || channels < 1 || channels > 2 || rate < 1000) return false;
    SoundWaveHeader h;
    memset(&h, 0, sizeof h);
    h.magic = RSND_MAGIC, h.version = RSND_VERSION;
    h.channels = (uint16_t)channels, h.sample_rate = rate, h.frames = frames;
    h.flags = stream ? SND_STREAM : 0;
    h.volume = 1.0f, h.pitch = 1.0f, h.priority = 1.0f;
    h.data_offset = sizeof h, h.data_bytes = frames * channels * 2u;
    measure(&h, pcm);
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = re_fopen(tmp, "wb");
    bool ok = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(pcm, 2, (size_t)frames * channels, f) == (size_t)frames * channels;
    if (f) ok = fclose(f) == 0 && ok;
    // already mapped (a reimport): its voices stopped, unmapped (Windows won't replace a mapped file), mapped again on
    // its next use
    char vp[600];
    vpath_of(path, vp, sizeof vp);
    uint32_t hh = audio_vpath_hash(vp);
    for (uint32_t i = 0; ok && i < W.n; i++)
        if (W.waves[i]->hash == hh && W.waves[i]->view) {
            audio_forget_wave(W.waves[i]);
            re_unmap_file(W.waves[i]->mapping);
            W.waves[i]->view = NULL, W.waves[i]->pcm = NULL, W.waves[i]->mapping = NULL;
        }
    ok = ok && re_replace_file(tmp, path); // (all or nothing: a crash mid-write keeps the old file)
    return ok;
}

static bool map_wave(SoundWave* s) {
    if (s->view) return true;
    const uint8_t* data = NULL;
    uint64_t size = 0;
    ReFileMap* m = re_map_file(s->path, &data, &size);
    if (!m) return false;
    SoundWaveHeader h;
    if (size < sizeof h) {
        re_unmap_file(m);
        return false;
    }
    memcpy(&h, data, sizeof h);
    bool ok = h.magic == RSND_MAGIC && h.version >= 1 && h.version <= RSND_VERSION && (h.channels == 1 || h.channels == 2) &&
              h.sample_rate >= 1000 && h.data_offset >= sizeof h && (uint64_t)h.data_offset + h.data_bytes <= size &&
              (uint64_t)h.frames * h.channels * 2u <= h.data_bytes;
    if (!ok) {
        re_unmap_file(m);
        printf("[RealEngine] Audio: %s is not a Sound Wave of this engine\n", s->path);
        return false;
    }
    s->h = h;
    s->mapping = m, s->view = data;
    s->pcm = (const int16_t*)(data + h.data_offset);
    s->length = (double)h.frames / h.sample_rate;
    if (h.loop_end > h.frames) s->h.loop_end = h.frames;
    if (s->h.loop_start >= (s->h.loop_end ? s->h.loop_end : h.frames)) s->h.loop_start = 0;
    return true;
}

SoundWave* audio_wave_load(const char* vpath) {
    uint32_t hh = audio_vpath_hash(vpath);
    for (uint32_t i = 0; i < W.n; i++)
        if (W.waves[i]->hash == hh) return map_wave(W.waves[i]) ? W.waves[i] : NULL;
    if (W.n >= MAX_WAVES) return NULL;
    SoundWave* s = calloc(1, sizeof *s); // (a load, not a frame; kept until the process ends: voices point at it)
    if (!s) return NULL;
    disk_of(vpath, s->path, sizeof s->path);
    s->hash = hh;
    const char* b = strrchr(vpath, '/');
    snprintf(s->name, sizeof s->name, "%s", b ? b + 1 : vpath);
    char* dot = strrchr(s->name, '.');
    if (dot) *dot = 0;
    if (!map_wave(s)) {
        free(s);
        return NULL;
    }
    W.waves[W.n++] = s;
    return s;
}

bool audio_wave_save(SoundWave* s) {
    if (!s || !s->view) return false;
    // the header in place (the samples stay): written through its own handle, the mapping sees it
    FILE* f = re_fopen(s->path, "r+b");
    if (!f) return false;
    bool ok = fwrite(&s->h, sizeof s->h, 1, f) == 1;
    ok = fclose(f) == 0 && ok;
    if (ok) s->dirty = false;
    return ok;
}

static void scan_dir(const char* disk, int depth);
static void scan_entry(void* user, const ReDirEntry* e) {
    const char* dir = user;
    char path[800], vpath[800];
    snprintf(path, sizeof path, "%s/%s", dir, e->name);
    if (e->is_dir) {
        scan_dir(path, 1);
        return;
    }
    const char* dot = strrchr(e->name, '.');
    if (!dot || _stricmp(dot, ".rsound")) return;
    vpath_of(path, vpath, sizeof vpath);
    audio_wave_load(vpath);
}
static void scan_dir(const char* disk, int depth) {
    if (depth > 12) return;
    re_dir_walk(disk, scan_entry, (void*)disk);
}
static void scan(void) {
    if (W.scanned || !W.root[0]) return;
    W.scanned = true;
    scan_dir(W.root, 0);
}

SoundWave* audio_wave(uint32_t hash) {
    if (!hash) return NULL;
    scan();
    for (uint32_t i = 0; i < W.n; i++)
        if (W.waves[i]->hash == hash) return map_wave(W.waves[i]) ? W.waves[i] : NULL;
    // a miss: the folder walked again (an import may have made it), at most every 2 s (a missing sound asked every
    // frame doesn't walk the disk every frame)
    static double last;
    double now = re_time_seconds();
    if (now - last < 2.0) return NULL;
    last = now;
    W.scanned = false;
    scan();
    for (uint32_t i = 0; i < W.n; i++)
        if (W.waves[i]->hash == hash) return map_wave(W.waves[i]) ? W.waves[i] : NULL;
    return NULL;
}

uint32_t audio_wave_count(void) {
    scan();
    return W.n;
}
SoundWave* audio_wave_at(uint32_t i) { return i < W.n ? W.waves[i] : NULL; }
void audio_rescan(void) { W.scanned = false; }
