// The Android game (notas-dev/android.md): the parts of platform.h that are Android's own; the rest is the POSIX branch
// of platform_win32.c. The packaged game's files (game.ini, the project's Content, the level files) are the APK's
// assets: assets/manifest.txt (written by android/build_apk.ps1) lists them; they are copied into the app's files
// folder when the game's version changed (or the folder is missing), and the game reads them there as on the PC.
// (Copied from the Tlender Mobile's android/platform_android.c and adapted: no file pickers, no crash-test intents.)
#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform.h"
#include "re_android.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "RealEngine", __VA_ARGS__)

static void make_dirs_for(const char* file) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", file);
    for (char* p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(tmp, 0770);
        *p = '/';
    }
}

// The game's build stamp (the APK's assets/build.txt: its version and the moment it was packed): a new APK extracts again
static void build_stamp(AAssetManager* am, char* out, size_t cap) {
    snprintf(out, cap, "%s", RE_VERSION_STRING);
    AAsset* a = AAssetManager_open(am, "build.txt", AASSET_MODE_BUFFER);
    if (!a) return;
    size_t n = (size_t)AAsset_getLength(a);
    if (n >= cap) n = cap - 1;
    memcpy(out, AAsset_getBuffer(a), n);
    out[n] = 0;
    for (char* c = out; *c; c++)
        if (*c == '\r' || *c == '\n') *c = 0;
    AAsset_close(a);
}

void re_android_extract_assets(void) {
    struct android_app* app = re_android_app();
    AAssetManager* am = app->activity->assetManager;
    char stamp_path[600], stamp[128] = "", want[128];
    build_stamp(am, want, sizeof want);
    snprintf(stamp_path, sizeof stamp_path, "%s/game/.build", re_android_files_dir());
    FILE* f = fopen(stamp_path, "rb");
    if (f) {
        size_t n = fread(stamp, 1, sizeof stamp - 1, f);
        stamp[n] = 0;
        fclose(f);
    }
    if (!strcmp(stamp, want)) return;
    AAsset* list = AAssetManager_open(am, "manifest.txt", AASSET_MODE_BUFFER);
    if (!list) {
        LOGI("sem assets/manifest.txt: nada a extrair");
        return;
    }
    size_t len = (size_t)AAsset_getLength(list);
    char* text = malloc(len + 1); // (once, at start)
    if (!text) return AAsset_close(list);
    memcpy(text, AAsset_getBuffer(list), len);
    text[len] = 0;
    AAsset_close(list);
    int count = 0;
    double bytes = 0.0;
    for (char* line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (!line[0]) continue;
        AAsset* a = AAssetManager_open(am, line, AASSET_MODE_STREAMING);
        if (!a) continue;
        char out[1200];
        snprintf(out, sizeof out, "%s/%s", re_android_files_dir(), line);
        make_dirs_for(out);
        FILE* o = fopen(out, "wb");
        if (o) {
            static char buf[256 * 1024];
            int r;
            while ((r = AAsset_read(a, buf, sizeof buf)) > 0) fwrite(buf, 1, (size_t)r, o), bytes += r;
            fclose(o);
            count++;
        }
        AAsset_close(a);
    }
    free(text);
    make_dirs_for(stamp_path);
    f = fopen(stamp_path, "wb");
    if (f) fputs(want, f), fclose(f);
    LOGI("%d arquivos do jogo extraídos (%.1f MB, build %s)", count, bytes / 1048576.0, want);
}

void re_android_read_test_extras(void) {}
