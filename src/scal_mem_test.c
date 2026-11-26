// (2026-11-25, bug-fixer) --scalability-mem-test: the UI sweep saw +270 MB of memory stay after Settings > Cinematic.
// Medium -> Cinematic -> Medium three times; after each switch (the frames settled) the GPU's memory this process uses
// (VK_EXT_memory_budget: the targets, the pipelines' memory on the GPU) and, for information, its private bytes. A leak
// grows each round. The private bytes alone moved with the machine's other work (+17 MB a round in the morning,
// +100..+130 with the same binaries while another session built), so the check is on the GPU's
#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include "editor.h"
#include "render.h"
#include "vkctx.h"

static size_t smt_private(void) {
    PROCESS_MEMORY_COUNTERS_EX pm;
    memset(&pm, 0, sizeof pm);
    pm.cb = sizeof pm;
    return K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm) ? pm.PrivateUsage : 0;
}

static struct {
    int step, frames;
    double gpu[8], priv[8]; // Medium, Cinematic, Medium, Cinematic, Medium, Cinematic, Medium (MB)
    double worst;           // the longest frame since the last switch (ms)
} T;

#define SMT_SETTLE 90 // frames after each switch (the targets made again, the pipelines compiled, the caches filled)

// true: finished (ok in *ok)
bool scal_mem_test_frame(Editor* ed, VkCtx* vk, double dt, bool* ok) {
    if (T.frames > 0 && dt * 1000.0 > T.worst) T.worst = dt * 1000.0; // (frame 1's dt: the switch's own frame)
    if (++T.frames < SMT_SETTLE) return false;
    T.frames = 0;
    double gpu = (double)vkctx_gpu_memory_used(vk) / 1048576.0, priv = (double)smt_private() / 1048576.0;
    printf("[RealEngine] Scalability memory: %s: GPU %.0f MB, private %.0f MB (the longest frame after the switch %.0f ms)\n",
           T.step & 1 ? "Cinematic" : "Medium", gpu, priv, T.step ? T.worst : 0.0);
    T.worst = 0.0;
    T.gpu[T.step] = gpu, T.priv[T.step] = priv, T.step++;
    if (T.step == 7) {
        if (!vk->mem_budget) { // (no VK_EXT_memory_budget: the private bytes, as before, told)
            printf("[RealEngine] Scalability memory: no VK_EXT_memory_budget on this GPU: the private bytes only (information)\n");
            *ok = true;
            printf("[RealEngine] Scalability memory test OK\n");
            return true;
        }
        // (the first Medium is the start's: its loading still settling; the leak is what each later round adds)
        double cine = T.gpu[3] - T.gpu[2], g1 = T.gpu[4] - T.gpu[2], g2 = T.gpu[6] - T.gpu[4];
        printf("[RealEngine] Scalability memory: GPU: Cinematic +%.0f MB over Medium; each later round to Cinematic and back %+.0f, %+.0f MB "
               "(private %+.0f, %+.0f: information)\n", cine, g1, g2, T.priv[4] - T.priv[2], T.priv[6] - T.priv[4]);
        *ok = g1 < 32.0 && g2 < 32.0;
        if (!*ok) printf("[RealEngine] FAIL: Scalability memory: each round to Cinematic and back keeps GPU memory (%+.0f, %+.0f MB)\n", g1, g2);
        else printf("[RealEngine] Scalability memory test OK\n");
        return true;
    }
    editor_set_scalability(ed, T.step & 1 ? RENDER_SCALABILITY_LEVELS - 1 : 1, 0.0);
    return false;
}
