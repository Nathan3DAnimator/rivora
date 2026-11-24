#include "vkctx.h"
#include "crashlog.h"
#include "platform.h"
#include "re_zlib.h"
#include "upscale_vendor.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

const char* vk_result_name(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "VkResult desconhecido";
    }
}

// --- GPU crash markers (gpu_mark) ---
// Per frame slot two words the GPU writes: [0] the last pass started (TOP_OF_PIPE), [1] the last one whose previous
// work is all done (ALL_COMMANDS); each value is (frame number & 0xFFFFF) << 12 | the pass's index in that frame
static VkCtx* g_mark_ctx;
static bool g_really_lost, g_wait_timed_out; // vk_check saw VK_ERROR_DEVICE_LOST from the driver (vkGetDeviceFaultInfoEXT may be asked)
static VkBuffer g_mark_buf;
static VkDeviceMemory g_mark_mem;
static volatile uint32_t* g_mark_ptr;
static PFN_vkCmdWriteBufferMarker2AMD g_mark_write;
static const char* g_mark_names[RE_FRAMES_IN_FLIGHT][RE_GPU_MARKS];
static uint32_t g_mark_count[RE_FRAMES_IN_FLIGHT];
static uint32_t g_mark_frame[RE_FRAMES_IN_FLIGHT]; // (frame number & 0xFFFFF) of the slot's names

// --- The GPU's time per pass (vkctx_pass_timing; --gpu-passes, the Debug Viewer's GPU Passes): each gpu_mark also a
// timestamp after all the work before it (ALL_COMMANDS), so a pass's time is from its mark to the next one's, the last
// one's to the frame's end. Unreal's "stat gpu" over the crash markers' names: nothing is written while off
#define PT_MAX 128u // timestamps per frame slot (the marks past it count in the one before)
#define PT_NAMES 160u
static VkQueryPool g_pt_pool;
static bool g_pt_on, g_pt_valid[RE_FRAMES_IN_FLIGHT];
static GpuPassStat g_pt[PT_NAMES];
static uint32_t g_pt_n, g_pt_frames;

void vkctx_pass_timing(bool on) { g_pt_on = on && g_pt_pool; }
bool vkctx_pass_timing_on(void) { return g_pt_on; }
uint32_t vkctx_pass_stats(const GpuPassStat** out, uint32_t* frames) {
    *out = g_pt;
    if (frames) *frames = g_pt_frames;
    return g_pt_n;
}
void vkctx_pass_stats_reset(void) {
    for (uint32_t i = 0; i < g_pt_n; i++) g_pt[i].ms = 0.0, g_pt[i].frames = 0, g_pt[i].last_ms = 0.0;
    g_pt_frames = 0;
}
static void pt_add(const char* name, double ms) {
    uint32_t i = 0;
    for (; i < g_pt_n; i++)
        if (g_pt[i].name == name || !strcmp(g_pt[i].name, name)) break;
    if (i == g_pt_n) {
        if (g_pt_n >= PT_NAMES) return;
        g_pt[g_pt_n].name = name, g_pt[g_pt_n].ms = 0.0, g_pt[g_pt_n].frames = 0, g_pt[g_pt_n].last_ms = 0.0, g_pt_n++;
    }
    if (g_pt[i].frames == g_pt_frames + 1u) g_pt[i].ms += ms, g_pt[i].last_ms += ms; // (the same pass twice in a frame: summed)
    else g_pt[i].ms += ms, g_pt[i].last_ms = ms, g_pt[i].frames++;
}
// slot f's last frame is done (vkctx_begin_frame waited for it): its marks' times into the sums
static void pt_read(VkCtx* ctx, uint32_t f) {
    if (!g_pt_valid[f]) return;
    g_pt_valid[f] = false;
    uint32_t n = g_mark_count[f] < PT_MAX ? g_mark_count[f] : PT_MAX;
    uint64_t t[PT_MAX + 1], te[2];
    if (!n || vkGetQueryPoolResults(ctx->device, g_pt_pool, f * PT_MAX, n, sizeof(uint64_t) * PT_MAX, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    if (!ctx->timestamps || vkGetQueryPoolResults(ctx->device, ctx->timestamps, f * 2, 2, sizeof te, te, sizeof te[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    t[n] = te[1];
    for (uint32_t k = 0; k < n; k++) pt_add(g_mark_names[f][k], t[k + 1] > t[k] ? (double)(t[k + 1] - t[k]) * ctx->timestamp_ms : 0.0);
    g_pt_frames++;
}

void gpu_mark(VkCommandBuffer cmd, const char* name) {
    VkCtx* ctx = g_mark_ctx;
    if ((!g_mark_write && !g_pt_on) || !ctx || cmd != vkctx_cmd(ctx)) return;
    uint32_t f = ctx->frame, k = g_mark_count[f];
    if (k >= RE_GPU_MARKS) return;
    g_mark_names[f][k] = name;
    g_mark_count[f] = k + 1;
    if (g_pt_on && k < PT_MAX) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_pt_pool, f * PT_MAX + k);
    if (!g_mark_write) return;
    uint32_t tag = g_mark_frame[f] << 12;
    VkDeviceSize at = (VkDeviceSize)f * 16;
    if (k) g_mark_write(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_mark_buf, at + 4, tag | (k - 1)); // the one before: done
    g_mark_write(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g_mark_buf, at, tag | k);                    // this one: started
}

// A finer pass's name for the per-pass timing only (no crash marker: each of those waits for the work before it, ~0.01 ms
// a mark): nothing while the timing is off
void gpu_mark_detail(VkCommandBuffer cmd, const char* name) {
    VkCtx* ctx = g_mark_ctx;
    if (!g_pt_on || !ctx || cmd != vkctx_cmd(ctx)) return;
    uint32_t f = ctx->frame, k = g_mark_count[f];
    if (k >= RE_GPU_MARKS || k >= PT_MAX) return;
    g_mark_names[f][k] = name;
    g_mark_count[f] = k + 1;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_pt_pool, f * PT_MAX + k);
}

static void marks_begin_frame(VkCtx* ctx) {
    uint32_t f = ctx->frame;
    pt_read(ctx, f);
    if (g_pt_on) vkCmdResetQueryPool(ctx->cmd[f], g_pt_pool, f * PT_MAX, PT_MAX), g_pt_valid[f] = true;
    g_mark_count[f] = 0;
    g_mark_frame[f] = (uint32_t)(ctx->frame_number + 1) & 0xFFFFFu; // (the timeline value this frame will signal)
    gpu_mark(ctx->cmd[f], "inicio do quadro");
}

static void marks_create(VkCtx* ctx) {
    g_mark_ctx = ctx;
    if (ctx->timestamps) { // (the per-pass timing's: made always, written only while on)
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = PT_MAX * RE_FRAMES_IN_FLIGHT;
        if (vkCreateQueryPool(ctx->device, &qi, NULL, &g_pt_pool) != VK_SUCCESS) g_pt_pool = VK_NULL_HANDLE;
        if (getenv("REALENGINE_GPU_PASSES")) g_pt_on = g_pt_pool != VK_NULL_HANDLE;
    }
    if (!ctx->gpu_markers) return;
    g_mark_write = (PFN_vkCmdWriteBufferMarker2AMD)vkGetDeviceProcAddr(ctx->device, "vkCmdWriteBufferMarker2AMD");
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = 16 * RE_FRAMES_IN_FLIGHT;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    void* p = NULL;
    if (!g_mark_write || vkCreateBuffer(ctx->device, &bi, NULL, &g_mark_buf) != VK_SUCCESS) goto fail;
    vkGetBufferMemoryRequirements(ctx->device, g_mark_buf, &mr);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = vkctx_find_memory(ctx, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(ctx->device, &ai, NULL, &g_mark_mem) != VK_SUCCESS || vkBindBufferMemory(ctx->device, g_mark_buf, g_mark_mem, 0) != VK_SUCCESS ||
        vkMapMemory(ctx->device, g_mark_mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS)
        goto fail;
    g_mark_ptr = p;
    memset(p, 0xFF, 16 * RE_FRAMES_IN_FLIGHT); // (nothing written yet)
    return;
fail:
    if (g_mark_buf) vkDestroyBuffer(ctx->device, g_mark_buf, NULL);
    if (g_mark_mem) vkFreeMemory(ctx->device, g_mark_mem, NULL);
    g_mark_buf = VK_NULL_HANDLE, g_mark_mem = VK_NULL_HANDLE, g_mark_write = NULL;
    ctx->gpu_markers = false;
}

static void marks_destroy(VkCtx* ctx) {
    if (g_pt_pool) vkDestroyQueryPool(ctx->device, g_pt_pool, NULL);
    g_pt_pool = VK_NULL_HANDLE, g_pt_on = false;
    if (g_mark_buf) vkDestroyBuffer(ctx->device, g_mark_buf, NULL);
    if (g_mark_mem) vkFreeMemory(ctx->device, g_mark_mem, NULL);
    g_mark_buf = VK_NULL_HANDLE, g_mark_mem = VK_NULL_HANDLE, g_mark_ptr = NULL, g_mark_write = NULL, g_mark_ctx = NULL;
}

// A written value's pass index in the names the slot holds (-1: nothing yet / an older frame's)
static int mark_index(uint32_t f, uint32_t v) {
    if (v == 0xFFFFFFFFu || (v >> 12) != g_mark_frame[f] || (v & 0xFFFu) >= g_mark_count[f]) return -1;
    return (int)(v & 0xFFFu);
}

bool vkctx_gpu_marks_last(const char** started, const char** finished, uint32_t* count) {
    if (!g_mark_ptr || !g_mark_ctx) return false;
    uint32_t f = (g_mark_ctx->frame + RE_FRAMES_IN_FLIGHT - 1) % RE_FRAMES_IN_FLIGHT; // the last submitted
    int a = mark_index(f, g_mark_ptr[f * 4]), b = mark_index(f, g_mark_ptr[f * 4 + 1]);
    *started = a >= 0 ? g_mark_names[f][a] : NULL;
    *finished = b >= 0 ? g_mark_names[f][b] : NULL;
    *count = g_mark_count[f];
    return true;
}

static void put(char* out, size_t cap, size_t* n, const char* fmt, ...) {
    if (!cap || *n + 1 >= cap) return;
    va_list a;
    va_start(a, fmt);
    int w = vsnprintf(out + *n, cap - *n, fmt, a);
    va_end(a);
    if (w > 0) *n += (size_t)w < cap - *n ? (size_t)w : cap - *n - 1;
}

void vkctx_gpu_diagnosis(char* full, size_t full_cap, char* where, size_t where_cap) {
    size_t n = 0, wn = 0;
    if (full_cap) full[0] = 0;
    if (where_cap) where[0] = 0;
    VkCtx* ctx = g_mark_ctx;
    if (!ctx) return;
    if (g_mark_ptr) {
        // the slots from the oldest frame in flight to the newest: the first pass started and not finished is where
        // the GPU stopped (a frame that reached its last mark finished)
        for (uint32_t i = 0; i < RE_FRAMES_IN_FLIGHT; i++) {
            uint32_t f = (ctx->frame + 1 + i) % RE_FRAMES_IN_FLIGHT;
            if (!g_mark_count[f]) continue;
            int a = mark_index(f, g_mark_ptr[f * 4]), b = mark_index(f, g_mark_ptr[f * 4 + 1]);
            put(full, full_cap, &n, "GPU, quadro %u (%u passes marcados): ultimo iniciado \"%s\", ultimo com tudo antes terminado \"%s\"\n",
                g_mark_frame[f], g_mark_count[f], a >= 0 ? g_mark_names[f][a] : "-", b >= 0 ? g_mark_names[f][b] : "-");
            if (a < 0 || a + 1 >= (int)g_mark_count[f]) continue; // not started / finished
            int first = b + 1 <= a ? b + 1 : a; // passes first..a were running
            for (int k = first; k <= a; k++) put(full, full_cap, &n, "  em andamento: %s\n", g_mark_names[f][k]);
            if (!wn) {
                put(where, where_cap, &wn, "A GPU parou no quadro %u em \"%s\"", g_mark_frame[f], g_mark_names[f][first]);
                if (first < a) put(where, where_cap, &wn, " (em andamento ate \"%s\")", g_mark_names[f][a]);
                put(where, where_cap, &wn, ".");
            }
        }
    }
    if (ctx->device_fault && g_really_lost) { // (only valid on a lost device: not after a wait that timed out, nor in the test)
        PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(ctx->device, "vkGetDeviceFaultInfoEXT");
        VkDeviceFaultCountsEXT c = { VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
        if (get && get(ctx->device, &c, NULL) == VK_SUCCESS) {
            static VkDeviceFaultAddressInfoEXT addr[16];
            static VkDeviceFaultVendorInfoEXT vend[16];
            if (c.addressInfoCount > 16) c.addressInfoCount = 16;
            if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
            c.vendorBinarySize = 0;
            VkDeviceFaultInfoEXT fi = { VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT };
            fi.pAddressInfos = c.addressInfoCount ? addr : NULL;
            fi.pVendorInfos = c.vendorInfoCount ? vend : NULL;
            VkResult r = get(ctx->device, &c, &fi);
            if (r == VK_SUCCESS || r == VK_INCOMPLETE) {
                static const char* const kinds[] = { "nenhum", "leitura invalida", "escrita invalida", "execucao invalida",
                                                     "instrucao desconhecida", "instrucao invalida", "instrucao (falha)" };
                put(full, full_cap, &n, "Falha do driver: %s\n", fi.description[0] ? fi.description : "(sem descricao)");
                for (uint32_t i = 0; i < c.addressInfoCount; i++) {
                    uint32_t t = (uint32_t)addr[i].addressType;
                    put(full, full_cap, &n, "  endereco 0x%llx (+-0x%llx): %s\n", (unsigned long long)addr[i].reportedAddress,
                        (unsigned long long)addr[i].addressPrecision, t < 7 ? kinds[t] : "?");
                }
                for (uint32_t i = 0; i < c.vendorInfoCount; i++)
                    put(full, full_cap, &n, "  %s (codigo 0x%llx, dado 0x%llx)\n", vend[i].description,
                        (unsigned long long)vend[i].vendorFaultCode, (unsigned long long)vend[i].vendorFaultData);
                if (c.addressInfoCount) {
                    uint32_t t = (uint32_t)addr[0].addressType;
                    put(where, where_cap, &wn, "%sFalha: %s em 0x%llx.", wn ? " " : "", t < 7 ? kinds[t] : "?",
                        (unsigned long long)addr[0].reportedAddress);
                }
            }
        }
    }
    if (!g_mark_ptr && !ctx->device_fault)
        put(full, full_cap, &n, "GPU: sem marcadores (VK_AMD_buffer_marker) nem VK_EXT_device_fault neste driver\n");
}

void vk_check(VkResult result, const char* what) {
    if (result == VK_SUCCESS) return;
    fprintf(stderr, "[RealEngine] Vulkan error %d (%s) em %s\n", (int)result, vk_result_name(result), what ? what : "?");
    if (result >= 0) return;
    // Fatal: a crash report says which call and why (the GPU gone, out of memory)
    const char* kind = result == VK_ERROR_DEVICE_LOST ? "The GPU stopped responding (VK_ERROR_DEVICE_LOST)"
                       : (result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY)
                           ? "Out of memory (Vulkan)"
                           : "Fatal Vulkan error";
    if (result == VK_ERROR_DEVICE_LOST) { // where the GPU stopped (the markers, the driver's fault info)
        static char diag[2048];
        g_really_lost = !g_wait_timed_out;
        char where[512];
        vkctx_gpu_diagnosis(diag, sizeof diag, where, sizeof where);
        if (diag[0]) re_log(RE_LOG_ERROR, "%s", diag);
        if (where[0]) crash_fatal(kind, "%s (%d) in %s. %s", vk_result_name(result), (int)result, what ? what : "?", where);
    }
    crash_fatal(kind, "%s (%d) in %s", vk_result_name(result), (int)result, what ? what : "?");
}

const char* g_vk_extra_instance_exts[16];
uint32_t g_vk_extra_ninst;
const char* g_vk_extra_device_exts[24];
uint32_t g_vk_extra_ndev;
VkPhysicalDevice (*g_vk_pick_gpu)(VkInstance instance);
int g_vk_validation_errors;

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT type,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    (void)type; (void)user;
    // (a warning, by design: the main pass's ray traced shaders write the shadow split's target, left without an image
    // when no area light needs it; VK_EXT_dynamic_rendering_unused_attachments drops those writes, render.c)
    if (severity < VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT && data->pMessage &&
        strstr(data->pMessage, "(imageView is VK_NULL_HANDLE) and this write is unused"))
        return VK_FALSE;
    // (a warning inside AMD's FSR 3.1.4 itself: its shader declares its own luma history Rgba8 and its backend makes that
    // image RGBA16F; the vendor's code, not the engine's: upscale_vendor.c)
    if (severity < VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT && data->pMessage && strstr(data->pMessage, "\"rw_luma_history\""))
        return VK_FALSE;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) fprintf(stderr, "[Vulkan] %s\n", data->pMessage);
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) g_vk_validation_errors++; // --bench fails on them
    return VK_FALSE;
}

static bool layer_available(const char* name) {
    VkLayerProperties layers[64];
    uint32_t count = 64;
    vkEnumerateInstanceLayerProperties(&count, layers);
    for (uint32_t i = 0; i < count; i++)
        if (strcmp(layers[i].layerName, name) == 0) return true;
    return false;
}

static bool instance_ext_available(const char* name) {
    VkExtensionProperties ex[256];
    uint32_t count = 256;
    vkEnumerateInstanceExtensionProperties(NULL, &count, ex);
    for (uint32_t i = 0; i < count; i++)
        if (!strcmp(ex[i].extensionName, name)) return true;
    return false;
}

static bool create_instance(VkCtx* ctx, bool validation) {
    uint32_t version = 0;
    vkEnumerateInstanceVersion(&version);
    if (version < VK_API_VERSION_1_3) {
        fprintf(stderr, "[RealEngine] O driver não tem Vulkan 1.3 (tem %u.%u).\n", VK_API_VERSION_MAJOR(version),
                VK_API_VERSION_MINOR(version));
        return false;
    }
    uint32_t glfw_ext_count = 0;
    const char** glfw_exts = glfwGetRequiredInstanceExtensions(&glfw_ext_count);
    if (!glfw_exts) {
        fprintf(stderr, "[RealEngine] GLFW não encontrou suporte a Vulkan.\n");
        return false;
    }
    const char* exts[40];
    uint32_t ext_count = 0;
    for (uint32_t i = 0; i < glfw_ext_count && ext_count < 15; i++) exts[ext_count++] = glfw_exts[i];
    for (uint32_t i = 0; i < g_vk_extra_ninst && ext_count < 38; i++) { // (VR: the runtime's, once each)
        bool have = false;
        for (uint32_t k = 0; k < ext_count; k++) have |= !strcmp(exts[k], g_vk_extra_instance_exts[i]);
        if (!have) exts[ext_count++] = g_vk_extra_instance_exts[i];
    }
    ups_vendor_instance_exts(exts, &ext_count, 38); // (DLSS's and XeSS's, when their runtime is beside the executable)
    // (A9.4) the HDR10 swapchain's color space, when the driver has it (asked for later: vkctx_recreate_swapchain)
    if (ext_count < 37 && instance_ext_available(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
        exts[ext_count++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, ctx->hdr_possible = true;

    const char* layer = "VK_LAYER_KHRONOS_validation";
    bool use_validation = validation && layer_available(layer);
    if (use_validation) exts[ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Rivora Engine Editor";
    app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.pEngineName = "Rivora Engine";
    app.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = ext_count;
    ci.ppEnabledExtensionNames = exts;
    if (use_validation) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layer;
    }
    if (vkCreateInstance(&ci, NULL, &ctx->instance) != VK_SUCCESS) {
        fprintf(stderr, "[RealEngine] vkCreateInstance falhou.\n");
        return false;
    }
    if (use_validation) {
        PFN_vkCreateDebugUtilsMessengerEXT create_messenger =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(ctx->instance, "vkCreateDebugUtilsMessengerEXT");
        if (create_messenger) {
            VkDebugUtilsMessengerCreateInfoEXT mi = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mi.pfnUserCallback = debug_callback;
            create_messenger(ctx->instance, &mi, NULL, &ctx->messenger);
        }
        printf("[RealEngine] Camada de validação Vulkan ativa.\n");
    }
    return true;
}

// The features the engine needs from Vulkan 1.2/1.3 core: dynamic rendering, synchronization2, timeline semaphores.
static bool gpu_has_required_features(VkPhysicalDevice gpu) {
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13 };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12 };
    vkGetPhysicalDeviceFeatures2(gpu, &f2);
    return f13.dynamicRendering && f13.synchronization2 && f12.timelineSemaphore;
}

static bool pick_gpu(VkCtx* ctx) {
    VkPhysicalDevice gpus[16];
    uint32_t count = 16;
    vkEnumeratePhysicalDevices(ctx->instance, &count, gpus);
    if (count == 0) {
        fprintf(stderr, "[RealEngine] Nenhuma GPU com Vulkan encontrada.\n");
        return false;
    }
    int best_score = -1;
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpus[i], &props);
        if (props.apiVersion < VK_API_VERSION_1_3 || !gpu_has_required_features(gpus[i])) continue;

        VkQueueFamilyProperties qprops[32];
        uint32_t qcount = 32;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qcount, qprops);
        int family = -1;
        for (uint32_t q = 0; q < qcount; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], q, ctx->surface, &present);
            if ((qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qprops[q].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) {
                family = (int)q;
                break;
            }
        }
        if (family < 0) continue;

        uint32_t ecount = 0;
        vkEnumerateDeviceExtensionProperties(gpus[i], NULL, &ecount, NULL);
        VkExtensionProperties* eprops = calloc(ecount ? ecount : 1, sizeof *eprops);
        vkEnumerateDeviceExtensionProperties(gpus[i], NULL, &ecount, eprops);
        bool has_swapchain = false;
        for (uint32_t e = 0; e < ecount; e++)
            if (strcmp(eprops[e].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) has_swapchain = true;
        free(eprops);
        if (!has_swapchain) continue;

        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                  : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > best_score) {
            best_score = score;
            ctx->gpu = gpus[i];
            ctx->queue_family = (uint32_t)family;
        }
    }
    if (best_score >= 0 && g_vk_pick_gpu) { // VR: the GPU the headset's runtime drives
        VkPhysicalDevice want = g_vk_pick_gpu(ctx->instance);
        for (uint32_t i = 0; want && i < count; i++)
            if (gpus[i] == want && want != ctx->gpu) {
                uint32_t qn = 32;
                VkQueueFamilyProperties qp[32];
                vkGetPhysicalDeviceQueueFamilyProperties(want, &qn, qp);
                for (uint32_t q = 0; q < qn; q++)
                    if (qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { ctx->gpu = want, ctx->queue_family = q; break; }
            }
    }
    if (best_score < 0) {
        fprintf(stderr, "[RealEngine] Nenhuma GPU com Vulkan 1.3 (dynamic rendering, synchronization2, timeline "
                        "semaphores) + apresentação.\n");
        return false;
    }
    vkGetPhysicalDeviceProperties(ctx->gpu, &ctx->gpu_props);
    // NVIDIA's driver version: the major in bits 22-31 (617.42 -> 617)
    uint32_t nv_major = ctx->gpu_props.vendorID == 0x10DE ? (ctx->gpu_props.driverVersion >> 22) & 0x3FFu : 0u;
    ctx->serialize_frames = (nv_major && nv_major < 617) || getenv("REALENGINE_SERIALIZE_FRAMES");
    if (ctx->serialize_frames)
        printf("[RealEngine] Driver NVIDIA anterior ao 617: quadros gravados em série (contorno de um crash do driver).\n");
    printf("[RealEngine] GPU: %s (Vulkan %u.%u.%u)\n", ctx->gpu_props.deviceName,
           VK_API_VERSION_MAJOR(ctx->gpu_props.apiVersion), VK_API_VERSION_MINOR(ctx->gpu_props.apiVersion),
           VK_API_VERSION_PATCH(ctx->gpu_props.apiVersion));
    return true;
}

static bool device_has_extension(VkPhysicalDevice gpu, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, NULL);
    VkExtensionProperties* p = calloc(n ? n : 1, sizeof *p);
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, p);
    bool found = false;
    for (uint32_t i = 0; i < n && !found; i++) found = strcmp(p[i].extensionName, name) == 0;
    free(p);
    return found;
}

static bool create_device(VkCtx* ctx) {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qis[2] = { { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO }, { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO } };
    VkDeviceQueueCreateInfo qi = qis[0];
    qi.queueFamilyIndex = ctx->queue_family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    qis[0] = qi;
    { // (A9.7c) a compute-only family for async compute (NVIDIA's, AMD's: work beside the graphics queue's)
        VkQueueFamilyProperties qp[32];
        uint32_t qn = 32;
        vkGetPhysicalDeviceQueueFamilyProperties(ctx->gpu, &qn, qp);
        for (uint32_t q = 0; q < qn && !ctx->async_ok; q++)
            if ((qp[q].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && q != ctx->queue_family &&
                !getenv("REALENGINE_NO_ASYNC_QUEUE")) {
                ctx->async_family = q, ctx->async_ok = true;
                qis[1].queueFamilyIndex = q, qis[1].queueCount = 1, qis[1].pQueuePriorities = &priority;
            }
    }

    // What the GPU has, then what the engine turns on (only what it uses or will use: bindless and GPU-driven draws)
    VkPhysicalDeviceVulkan13Features h13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features h12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &h13 };
    VkPhysicalDeviceFeatures2 h2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &h12 };
    vkGetPhysicalDeviceFeatures2(ctx->gpu, &h2);

    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.maintenance4 = h13.maintenance4;
    f13.shaderDemoteToHelperInvocation = h13.shaderDemoteToHelperInvocation; // discard in SPIR-V 1.6
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13 };
    f12.timelineSemaphore = VK_TRUE;
    f12.descriptorIndexing = h12.descriptorIndexing;
    f12.runtimeDescriptorArray = h12.runtimeDescriptorArray;
    f12.descriptorBindingPartiallyBound = h12.descriptorBindingPartiallyBound;
    f12.descriptorBindingVariableDescriptorCount = h12.descriptorBindingVariableDescriptorCount;
    f12.descriptorBindingSampledImageUpdateAfterBind = h12.descriptorBindingSampledImageUpdateAfterBind;
    // (the materials' set: texture streaming swaps textures while frames are in flight)
    f12.descriptorBindingStorageImageUpdateAfterBind = h12.descriptorBindingStorageImageUpdateAfterBind;
    f12.descriptorBindingUpdateUnusedWhilePending = h12.descriptorBindingUpdateUnusedWhilePending;
    f12.shaderSampledImageArrayNonUniformIndexing = h12.shaderSampledImageArrayNonUniformIndexing;
    f12.bufferDeviceAddress = h12.bufferDeviceAddress;
    f12.scalarBlockLayout = h12.scalarBlockLayout;
    f12.drawIndirectCount = h12.drawIndirectCount;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12 };
    f2.features.multiDrawIndirect = h2.features.multiDrawIndirect;
    f2.features.drawIndirectFirstInstance = h2.features.drawIndirectFirstInstance;
    f2.features.samplerAnisotropy = h2.features.samplerAnisotropy;
    f2.features.textureCompressionBC = h2.features.textureCompressionBC;
    f2.features.shaderInt64 = h2.features.shaderInt64;
    f2.features.fillModeNonSolid = h2.features.fillModeNonSolid; // the Wireframe view mode
    f2.features.largePoints = h2.features.largePoints; // the particles' small sprites as points (particles.c)
    ctx->large_points = h2.features.largePoints != 0;
    f2.features.fragmentStoresAndAtomics = h2.features.fragmentStoresAndAtomics; // (Q7) the asset previews' AO depth pass
    ctx->frag_stores = h2.features.fragmentStoresAndAtomics != 0;
    ctx->bc_textures = h2.features.textureCompressionBC != 0;
    f2.features.independentBlend = h2.features.independentBlend; // the main pass's second target (the shadow split, render.c)
    f2.features.geometryShader = h2.features.geometryShader; // gl_PrimitiveID in mesh.frag (the Triangles view)

    // Hardware ray tracing (ray queries: the GI, the path tracer, the reflections) when the GPU has it
    const char* exts[48] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    uint32_t ext_count = 1;
    for (uint32_t i = 0; i < g_vk_extra_ndev && ext_count < 24; i++) // (VR: the runtime's, when the GPU has them)
        if (device_has_extension(ctx->gpu, g_vk_extra_device_exts[i]) && strcmp(g_vk_extra_device_exts[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            exts[ext_count++] = g_vk_extra_device_exts[i];
    VkPhysicalDeviceAccelerationStructureFeaturesKHR has_as = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    // the fragment's barycentrics and its triangle's vertices (mesh.frag RE_RT: the shadow rays leave from the curved
    // surface the vertex normals describe, not the flat triangle: no blocks of self-shadow on low-poly characters)
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR has_bc = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR };
    has_as.pNext = &has_bc;
    VkPhysicalDeviceRayQueryFeaturesKHR has_rq = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, &has_as };
    VkPhysicalDeviceFeatures2 hrt = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &has_rq };
    bool rt_exts = device_has_extension(ctx->gpu, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                   device_has_extension(ctx->gpu, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                   device_has_extension(ctx->gpu, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) &&
                   device_has_extension(ctx->gpu, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
    if (rt_exts) vkGetPhysicalDeviceFeatures2(ctx->gpu, &hrt);
    VkPhysicalDeviceAccelerationStructureFeaturesKHR f_as = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR f_bc = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR };
    f_as.pNext = &f_bc;
    VkPhysicalDeviceRayQueryFeaturesKHR f_rq = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, &f_as };
    ctx->ray_query = rt_exts && has_as.accelerationStructure && has_rq.rayQuery && h12.bufferDeviceAddress && h2.features.shaderInt64 &&
                     has_bc.fragmentShaderBarycentric && !getenv("REALENGINE_NO_RT"); // --no-rt: the fallback without ray tracing, tested on any GPU
    if (ctx->ray_query) {
        exts[ext_count++] = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
        exts[ext_count++] = VK_KHR_RAY_QUERY_EXTENSION_NAME;
        exts[ext_count++] = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
        exts[ext_count++] = VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME;
        f_bc.fragmentShaderBarycentric = VK_TRUE;
        f_as.accelerationStructure = VK_TRUE;
        f_rq.rayQuery = VK_TRUE;
        f13.pNext = &f_rq;
    }
    // Opacity micromaps (gi_omm.c, notas-dev/render.md "Opacity micromaps"): a Masked mesh's triangles split in micro
    // triangles marked opaque, transparent or unknown by their alpha; the rays test only the unknown ones' alpha
    VkPhysicalDeviceOpacityMicromapFeaturesEXT has_omm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT };
    static VkPhysicalDeviceOpacityMicromapFeaturesEXT f_omm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT };
    // (opt-in, REALENGINE_OMM=1: on the RTX 3060 it measured no gain, notas-dev/render.md; the RTX 40 has the hardware)
    if (ctx->ray_query && device_has_extension(ctx->gpu, VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME) && getenv("REALENGINE_OMM")) {
        VkPhysicalDeviceFeatures2 ho = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &has_omm };
        vkGetPhysicalDeviceFeatures2(ctx->gpu, &ho);
        if (has_omm.micromap) {
            ctx->omm = true;
            exts[ext_count++] = VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME;
            f_omm.micromap = VK_TRUE;
            f_omm.pNext = f_rq.pNext, f_rq.pNext = &f_omm;
        }
    }
    // Cluster acceleration structures (A8, NVIDIA's Mega Geometry; rt_clusters.c): the static meshes' BLAS made of
    // clusters of up to 128 triangles, quantized and compressed by the driver. REALENGINE_NO_CLAS=1: the classic BLAS
#ifdef VK_NV_cluster_acceleration_structure // (the NDK's headers are older: no clusters on Android, which has no ray tracing)
    VkPhysicalDeviceClusterAccelerationStructureFeaturesNV has_clas = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_FEATURES_NV };
    static VkPhysicalDeviceClusterAccelerationStructureFeaturesNV f_clas = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_FEATURES_NV };
    if (ctx->ray_query && device_has_extension(ctx->gpu, VK_NV_CLUSTER_ACCELERATION_STRUCTURE_EXTENSION_NAME) && !getenv("REALENGINE_NO_CLAS")) {
        VkPhysicalDeviceFeatures2 hc = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &has_clas };
        vkGetPhysicalDeviceFeatures2(ctx->gpu, &hc);
        VkPhysicalDeviceClusterAccelerationStructurePropertiesNV cp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_PROPERTIES_NV };
        VkPhysicalDeviceProperties2 pp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &cp };
        vkGetPhysicalDeviceProperties2(ctx->gpu, &pp);
        if (has_clas.clusterAccelerationStructure && cp.maxTrianglesPerCluster >= 64 && cp.maxVerticesPerCluster >= 64) {
            ctx->clas = true;
            ctx->clas_max_tris = cp.maxTrianglesPerCluster, ctx->clas_max_verts = cp.maxVerticesPerCluster;
            ctx->clas_max_geometry = cp.maxClusterGeometryIndex, ctx->clas_align = cp.clusterByteAlignment;
            ctx->clas_blas_align = cp.clusterBottomLevelByteAlignment, ctx->clas_scratch_align = cp.clusterScratchByteAlignment;
            exts[ext_count++] = VK_NV_CLUSTER_ACCELERATION_STRUCTURE_EXTENSION_NAME;
            f_clas.clusterAccelerationStructure = VK_TRUE;
            f_clas.pNext = f_rq.pNext, f_rq.pNext = &f_clas;
        }
    }
#endif
    printf("[RealEngine] Ray tracing por hardware: %s%s\n", ctx->ray_query ? "sim" : "nao", ctx->clas ? " (clusters: VK_NV_cluster_acceleration_structure)" : "");
    // --shader-stats: what the driver made of the shaders (registers: a pixel's occupancy). The main pass's shader sits near
    // a register step: a few lines more cost 10% of it (notas-dev/render.md), so a change is checked by its count
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR f_pe = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR };
    ctx->shader_stats = getenv("REALENGINE_SHADER_STATS") && device_has_extension(ctx->gpu, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    if (ctx->shader_stats) {
        exts[ext_count++] = VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME;
        f_pe.pipelineExecutableInfo = VK_TRUE;
        f_pe.pNext = f2.pNext, f2.pNext = &f_pe;
    }
    // the main pass's second target left without an image when it isn't used (render.c: the shadow split; without the
    // extension the split stays off and the main pass keeps one target)
    VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT f_ua = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT };
    if (device_has_extension(ctx->gpu, VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME)) {
        VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT q = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT };
        VkPhysicalDeviceFeatures2 qf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q };
        vkGetPhysicalDeviceFeatures2(ctx->gpu, &qf);
        if (q.dynamicRenderingUnusedAttachments) {
            exts[ext_count++] = VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME;
            f_ua.dynamicRenderingUnusedAttachments = VK_TRUE;
            f_ua.pNext = f2.pNext, f2.pNext = &f_ua;
            ctx->unused_attachments = true;
        }
    }
    // Variable rate shading by a rate image (render.c, A9.7b: coarser shading where last frame showed little detail):
    // the attachment and the pipeline's rate, a 16 px texel (or the GPU's smallest above it); REALENGINE_NO_VRS=1 off
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR f_vrs = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
    if (!getenv("REALENGINE_NO_VRS") && device_has_extension(ctx->gpu, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)) {
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR q = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
        VkPhysicalDeviceFeatures2 qf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q };
        vkGetPhysicalDeviceFeatures2(ctx->gpu, &qf);
        VkPhysicalDeviceFragmentShadingRatePropertiesKHR pr = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR };
        VkPhysicalDeviceProperties2 pp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pr };
        vkGetPhysicalDeviceProperties2(ctx->gpu, &pp);
        uint32_t texel = 16;
        if (texel < pr.minFragmentShadingRateAttachmentTexelSize.width) texel = pr.minFragmentShadingRateAttachmentTexelSize.width;
        if (q.pipelineFragmentShadingRate && q.attachmentFragmentShadingRate && texel <= pr.maxFragmentShadingRateAttachmentTexelSize.width &&
            texel <= pr.maxFragmentShadingRateAttachmentTexelSize.height && texel >= pr.minFragmentShadingRateAttachmentTexelSize.height) {
            exts[ext_count++] = VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME;
            f_vrs.pipelineFragmentShadingRate = f_vrs.attachmentFragmentShadingRate = VK_TRUE;
            f_vrs.pNext = f2.pNext, f2.pNext = &f_vrs;
            ctx->vrs = true, ctx->vrs_texel = texel;
        }
    }
    // Where the GPU stopped when it is lost (gpu_mark, above): VK_AMD_buffer_marker writes each pass's number into host
    // memory as the GPU starts and ends it; VK_EXT_device_fault gives the faulting address and the driver's reason
    // (both on the RTX 3060's driver; REALENGINE_NO_GPU_MARKERS=1 turns them off, to measure)
    VkPhysicalDeviceFaultFeaturesEXT f_fault = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
    if (!getenv("REALENGINE_NO_GPU_MARKERS")) {
        if (device_has_extension(ctx->gpu, VK_AMD_BUFFER_MARKER_EXTENSION_NAME)) {
            exts[ext_count++] = VK_AMD_BUFFER_MARKER_EXTENSION_NAME;
            ctx->gpu_markers = true;
        }
        if (device_has_extension(ctx->gpu, VK_EXT_DEVICE_FAULT_EXTENSION_NAME)) {
            VkPhysicalDeviceFaultFeaturesEXT q = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
            VkPhysicalDeviceFeatures2 qf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q };
            vkGetPhysicalDeviceFeatures2(ctx->gpu, &qf);
            if (q.deviceFault) {
                exts[ext_count++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
                f_fault.deviceFault = VK_TRUE;
                f_fault.pNext = f2.pNext, f2.pNext = &f_fault;
                ctx->device_fault = true;
            }
        }
    }
    // the vendors' upscalers (upscale_vendor.c): DLSS's and XeSS's device extensions, XeSS's features patched into the chain
    void* chain = &f2;
    ups_vendor_device_exts(ctx->instance, ctx->gpu, exts, &ext_count, 48, &chain);
    VkDeviceCreateInfo ci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, chain };
    ci.queueCreateInfoCount = ctx->async_ok ? 2 : 1;
    ci.pQueueCreateInfos = qis;
    ci.enabledExtensionCount = ext_count;
    ci.ppEnabledExtensionNames = exts;
    if (vkCreateDevice(ctx->gpu, &ci, NULL, &ctx->device) != VK_SUCCESS) {
        fprintf(stderr, "[RealEngine] vkCreateDevice falhou.\n");
        return false;
    }
    vkGetDeviceQueue(ctx->device, ctx->queue_family, 0, &ctx->queue);
    if (ctx->async_ok) vkGetDeviceQueue(ctx->device, ctx->async_family, 0, &ctx->async_queue);
    ups_vendor_device_ready(ctx);
#ifdef __ANDROID__
    { // (the 1.2 / 1.3 functions Android's libvulkan doesn't export: android/vk_android.c)
        void re_vk_android_load(VkDevice d);
        int re_vk_android_ready(void);
        re_vk_android_load(ctx->device);
        if (!re_vk_android_ready()) {
            fprintf(stderr, "[RealEngine] Vulkan 1.3 (dynamic rendering, synchronization2) indisponível neste aparelho.\n");
            return false;
        }
    }
#endif
    return true;
}

static void choose_format(VkCtx* ctx) {
    VkSurfaceFormatKHR formats[64];
    uint32_t count = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx->gpu, ctx->surface, &count, formats);
    // UNORM keeps ImGui colors as designed (the UI is authored in display space); the scene is tone mapped into it.
    ctx->color_format = count ? formats[0].format : VK_FORMAT_B8G8R8A8_UNORM;
    ctx->color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, ctx->hdr = false;
    if (ctx->want_hdr && ctx->hdr_possible) // (A9.4) HDR10: 10 bits a channel, PQ (ST.2084), BT.2020 primaries
        for (uint32_t i = 0; i < count; i++)
            if ((formats[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || formats[i].format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) &&
                formats[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
                ctx->color_format = formats[i].format, ctx->color_space = formats[i].colorSpace, ctx->hdr = true;
                return;
            }
    for (uint32_t i = 0; i < count; i++) {
        if ((formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            ctx->color_format = formats[i].format;
            break;
        }
    }
}

uint32_t vkctx_find_memory(VkCtx* ctx, uint32_t type_bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(ctx->gpu, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) return i;
    fprintf(stderr, "[RealEngine] Nenhum tipo de memória compatível.\n");
    abort();
}

static void destroy_swapchain_resources(VkCtx* ctx) {
    for (uint32_t i = 0; i < ctx->image_count; i++) {
        if (ctx->views[i]) vkDestroyImageView(ctx->device, ctx->views[i], NULL);
        if (ctx->render_done[i]) vkDestroySemaphore(ctx->device, ctx->render_done[i], NULL);
        ctx->views[i] = VK_NULL_HANDLE;
        ctx->render_done[i] = VK_NULL_HANDLE;
    }
}

static VkPresentModeKHR choose_present_mode(VkCtx* ctx) {
    if (ctx->vsync) return VK_PRESENT_MODE_FIFO_KHR; // always available
    VkPresentModeKHR modes[16];
    uint32_t n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx->gpu, ctx->surface, &n, modes);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < n; i++) // no tearing first, then immediate
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR && mode == VK_PRESENT_MODE_FIFO_KHR) mode = modes[i];
    for (uint32_t i = 0; i < n; i++)
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
    return mode;
}

bool vkctx_recreate_swapchain(VkCtx* ctx) {
    int w = 0, h = 0;
    glfwGetFramebufferSize(ctx->window, &w, &h);
    if (w <= 0 || h <= 0 || !ctx->surface) return false; // (no window: in the background)

    vkDeviceWaitIdle(ctx->device);
    destroy_swapchain_resources(ctx);
    if (ctx->want_hdr != ctx->hdr) choose_format(ctx); // (A9.4: HDR10 asked for or given up: the format again)

    VkSurfaceCapabilitiesKHR caps;
    vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->gpu, ctx->surface, &caps), "surface caps");
    if (caps.currentExtent.width != UINT32_MAX) {
        ctx->extent = caps.currentExtent;
    } else {
        ctx->extent.width = (uint32_t)w;
        ctx->extent.height = (uint32_t)h;
        if (ctx->extent.width < caps.minImageExtent.width) ctx->extent.width = caps.minImageExtent.width;
        if (ctx->extent.height < caps.minImageExtent.height) ctx->extent.height = caps.minImageExtent.height;
        if (ctx->extent.width > caps.maxImageExtent.width) ctx->extent.width = caps.maxImageExtent.width;
        if (ctx->extent.height > caps.maxImageExtent.height) ctx->extent.height = caps.maxImageExtent.height;
    }
    if (ctx->extent.width == 0 || ctx->extent.height == 0) return false;

    ctx->present_mode = choose_present_mode(ctx);
    // Mailbox needs one image more than the minimum to never wait for one
    uint32_t images = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && images > caps.maxImageCount) images = caps.maxImageCount;
    if (images > RE_MAX_SWAP_IMAGES) images = RE_MAX_SWAP_IMAGES;
    ctx->min_image_count = caps.minImageCount < 2 ? 2 : caps.minImageCount;

    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = ctx->surface;
    ci.minImageCount = images;
    ci.imageFormat = ctx->color_format;
    ci.imageColorSpace = ctx->color_space ? ctx->color_space : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = ctx->extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // (src: vkctx_request_shot)
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = ctx->present_mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = ctx->swapchain;

    VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
    vk_check(vkCreateSwapchainKHR(ctx->device, &ci, NULL, &new_swapchain), "vkCreateSwapchainKHR");
    if (ctx->swapchain) vkDestroySwapchainKHR(ctx->device, ctx->swapchain, NULL);
    ctx->swapchain = new_swapchain;

    ctx->image_count = RE_MAX_SWAP_IMAGES;
    vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &ctx->image_count, ctx->images);

    for (uint32_t i = 0; i < ctx->image_count; i++) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = ctx->images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ctx->color_format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        vk_check(vkCreateImageView(ctx->device, &vi, NULL, &ctx->views[i]), "vkCreateImageView(swapchain)");
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vk_check(vkCreateSemaphore(ctx->device, &si, NULL, &ctx->render_done[i]), "vkCreateSemaphore");
    }
    ctx->swapchain_dirty = false;
    return true;
}

// --- The pipeline cache on disk: a program the driver has never seen (a game packaged a minute ago, at a new path)
// compiled every shader on its first run; with the cache any program made with the engine starts warm ---
#define PIPELINE_CACHE_MAX ((long)64 << 20) // past it, mostly old shaders' entries: start over

static bool pipeline_cache_path(char* out, size_t cap) {
    const char* over = getenv("REALENGINE_PIPELINE_CACHE"); // the tests: a cold start, a cache of their own
    if (over && over[0]) return snprintf(out, cap, "%s", over) < (int)cap;
    char dir[1024];
    return re_cache_dir(dir, sizeof dir) && snprintf(out, cap, "%s/pipelines.bin", dir) < (int)cap;
}

// Data only for this GPU and driver (VkPipelineCacheHeaderVersionOne): another one's is never handed to the driver
static bool pipeline_cache_matches(const VkCtx* ctx, const uint8_t* d, size_t n) {
    uint32_t h[4];
    if (n < 16 + VK_UUID_SIZE) return false;
    memcpy(h, d, sizeof h);
    return h[0] >= 16 + VK_UUID_SIZE && h[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && h[2] == ctx->gpu_props.vendorID &&
           h[3] == ctx->gpu_props.deviceID && !memcmp(d + 16, ctx->gpu_props.pipelineCacheUUID, VK_UUID_SIZE);
}

static void pipeline_cache_create(VkCtx* ctx) {
    char path[1100];
    uint8_t* data = NULL;
    size_t n = 0;
    // (REALENGINE_PIPELINE_CACHE=off: a cold start measured, the file neither read nor written: the batches' test)
    const char* off = getenv("REALENGINE_PIPELINE_CACHE");
    ctx->pipeline_cache_off = off && !strcmp(off, "off");
    FILE* f = !ctx->pipeline_cache_off && pipeline_cache_path(path, sizeof path) ? re_fopen(path, "rb") : NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size > 0 && size <= PIPELINE_CACHE_MAX && (data = malloc((size_t)size)) != NULL && // once, at startup
            fread(data, 1, (size_t)size, f) == (size_t)size && pipeline_cache_matches(ctx, data, (size_t)size))
            n = (size_t)size;
        fclose(f);
    }
    VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    ci.initialDataSize = n;
    ci.pInitialData = n ? data : NULL;
    if (vkCreatePipelineCache(ctx->device, &ci, NULL, &ctx->pipeline_cache) != VK_SUCCESS) {
        ci.initialDataSize = 0, ci.pInitialData = NULL, n = 0; // data the driver refuses: an empty cache
        if (vkCreatePipelineCache(ctx->device, &ci, NULL, &ctx->pipeline_cache) != VK_SUCCESS) ctx->pipeline_cache = VK_NULL_HANDLE;
    }
    ctx->pipeline_cache_loaded = ctx->pipeline_cache_saved = n;
    free(data);
}

void vkctx_pipeline_cache_save(VkCtx* ctx) {
    size_t n = 0;
    if (!ctx->pipeline_cache || ctx->pipeline_cache_off || vkGetPipelineCacheData(ctx->device, ctx->pipeline_cache, &n, NULL) != VK_SUCCESS || n == 0 ||
        n == ctx->pipeline_cache_saved)
        return;
    char path[1100], tmp[1120];
    void* data = malloc(n); // at startup (after the renderer) and at exit: never in a frame
    if (!data || !pipeline_cache_path(path, sizeof path)) { free(data); return; }
    if (vkGetPipelineCacheData(ctx->device, ctx->pipeline_cache, &n, data) == VK_SUCCESS) {
        // written beside it and moved over: two programs saving at once never leave half a file
        snprintf(tmp, sizeof tmp, "%s.%u.tmp", path, re_thread_id() /* unique system-wide while it runs */);
        FILE* f = re_fopen(tmp, "wb");
        bool ok = f && fwrite(data, 1, n, f) == n;
        if (f) ok = fclose(f) == 0 && ok;
        if (ok && re_replace_file(tmp, path)) ctx->pipeline_cache_saved = n;
        else re_remove_file(tmp);
    }
    free(data);
}

#ifdef __ANDROID__
#include "../android/re_android.h"
// The window goes away when the game leaves the screen (another app, the home button): the swapchain and the surface
// first; they come back with the new window (notas-dev/android.md)
static void android_surface_lost(void* user) {
    VkCtx* ctx = user;
    if (!ctx->device) return;
    vkDeviceWaitIdle(ctx->device);
    destroy_swapchain_resources(ctx);
    if (ctx->swapchain) vkDestroySwapchainKHR(ctx->device, ctx->swapchain, NULL);
    if (ctx->surface) vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
    ctx->swapchain = VK_NULL_HANDLE;
    ctx->surface = VK_NULL_HANDLE;
    ctx->swapchain_dirty = true;
}
static void android_surface_gained(void* user) {
    VkCtx* ctx = user;
    if (!ctx->instance || ctx->surface) return;
    if (glfwCreateWindowSurface(ctx->instance, ctx->window, NULL, &ctx->surface) != VK_SUCCESS) ctx->surface = VK_NULL_HANDLE;
    ctx->swapchain_dirty = true;
}
#endif

bool vkctx_init(VkCtx* ctx, GLFWwindow* window, bool validation) {
    bool vsync = ctx->vsync; // the caller's choice survives the reset
    memset(ctx, 0, sizeof *ctx);
    ctx->window = window;
    ctx->vsync = vsync;
    ctx->clear_color[0] = ctx->clear_color[1] = ctx->clear_color[2] = 0.08f;

    if (!create_instance(ctx, validation)) return false;
    if (glfwCreateWindowSurface(ctx->instance, window, NULL, &ctx->surface) != VK_SUCCESS) {
        fprintf(stderr, "[RealEngine] Não foi possível criar a superfície da janela.\n");
        return false;
    }
#ifdef __ANDROID__
    re_android_set_surface_callbacks(android_surface_lost, android_surface_gained, ctx);
#endif
    if (!pick_gpu(ctx)) return false;
    if (!create_device(ctx)) return false;
    choose_format(ctx);
    pipeline_cache_create(ctx);

    // The one-off submissions (uploads, builds: gpu_begin_once) in a pool of their own: never in a frame slot's pool,
    // which is reset every frame and may hold the command buffer being recorded
    {
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = ctx->queue_family;
        vk_check(vkCreateCommandPool(ctx->device, &pi, NULL, &ctx->once_pool), "vkCreateCommandPool");
    }
    // One pool per frame slot: reset whole each frame (cheaper than resetting single command buffers)
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++) {
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = ctx->queue_family;
        vk_check(vkCreateCommandPool(ctx->device, &pi, NULL, &ctx->cmd_pool[i]), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = ctx->cmd_pool[i];
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vk_check(vkAllocateCommandBuffers(ctx->device, &ai, &ctx->cmd[i]), "vkAllocateCommandBuffers");
        vk_check(vkAllocateCommandBuffers(ctx->device, &ai, &ctx->cmd2[i]), "vkAllocateCommandBuffers"); // (A9.7c: a split frame's rest)
        if (ctx->async_ok) { // (A9.7c) the async compute's pool and command buffer of this slot
            VkCommandPoolCreateInfo api = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            api.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            api.queueFamilyIndex = ctx->async_family;
            vk_check(vkCreateCommandPool(ctx->device, &api, NULL, &ctx->async_pool[i]), "vkCreateCommandPool(async)");
            VkCommandBufferAllocateInfo aai = ai;
            aai.commandPool = ctx->async_pool[i];
            vk_check(vkAllocateCommandBuffers(ctx->device, &aai, &ctx->async_cmd[i]), "vkAllocateCommandBuffers(async)");
        }
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vk_check(vkCreateSemaphore(ctx->device, &si, NULL, &ctx->image_ready[i]), "vkCreateSemaphore");
    }
    VkSemaphoreTypeCreateInfo ti = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    ti.initialValue = 0;
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti };
    vk_check(vkCreateSemaphore(ctx->device, &si, NULL, &ctx->timeline), "vkCreateSemaphore(timeline)");
    if (ctx->async_ok) vk_check(vkCreateSemaphore(ctx->device, &si, NULL, &ctx->async_timeline), "vkCreateSemaphore(async)");

    // GPU timestamps, when the queue has them
    VkQueueFamilyProperties qprops[32];
    uint32_t qcount = 32;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->gpu, &qcount, qprops);
    if (ctx->queue_family < qcount && qprops[ctx->queue_family].timestampValidBits > 0) {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * RE_FRAMES_IN_FLIGHT;
        vk_check(vkCreateQueryPool(ctx->device, &qi, NULL, &ctx->timestamps), "vkCreateQueryPool");
        ctx->timestamp_ms = (double)ctx->gpu_props.limits.timestampPeriod / 1e6;
    }

    marks_create(ctx);

    if (!vkctx_recreate_swapchain(ctx)) ctx->swapchain_dirty = true; // minimized: created on the first visible frame
    return true;
}

void vkctx_shutdown(VkCtx* ctx) {
    if (!ctx->device) return;
    vkDeviceWaitIdle(ctx->device);
    ups_vendor_shutdown(ctx);
    vkctx_pipeline_cache_save(ctx); // the driver may have added what it compiled since (its background optimizer)
    if (ctx->pipeline_cache) vkDestroyPipelineCache(ctx->device, ctx->pipeline_cache, NULL);
    destroy_swapchain_resources(ctx);
    if (ctx->swapchain) vkDestroySwapchainKHR(ctx->device, ctx->swapchain, NULL);
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(ctx->device, ctx->image_ready[i], NULL);
        vkDestroyCommandPool(ctx->device, ctx->cmd_pool[i], NULL);
        if (ctx->async_pool[i]) vkDestroyCommandPool(ctx->device, ctx->async_pool[i], NULL);
    }
    if (ctx->async_timeline) vkDestroySemaphore(ctx->device, ctx->async_timeline, NULL);
    vkDestroyCommandPool(ctx->device, ctx->once_pool, NULL);
    vkDestroySemaphore(ctx->device, ctx->timeline, NULL);
    if (ctx->timestamps) vkDestroyQueryPool(ctx->device, ctx->timestamps, NULL);
    marks_destroy(ctx);
    vkDestroyDevice(ctx->device, NULL);
    if (ctx->surface) vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
    if (ctx->messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(ctx->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy_messenger) destroy_messenger(ctx->instance, ctx->messenger, NULL);
    }
    vkDestroyInstance(ctx->instance, NULL);
    memset(ctx, 0, sizeof *ctx);
}

// The last frame done on the GPU before the next one is recorded, only on NVIDIA drivers before 617 (serialize_frames):
// 596.36's background shader compiler crashed ("[vkbgc]", fixed in 617.42) and waiting made it much rarer
// (notas-dev/escala.md). Elsewhere frames overlap: 15% faster in the default level (471 against 411 fps).
// Waits for frame v on the timeline, at most RE_GPU_WAIT_S: a frame's work never takes that long (Windows resets a GPU
// stuck for 2 s), so past it the GPU is hung. The NVIDIA driver resets the channel after a page fault without always
// returning VK_ERROR_DEVICE_LOST (measured: the wait never returned, notas-dev/servicos.md): then it is reported here
// as a device lost, with the passes the markers say were running
#define RE_GPU_WAIT_S 10
static void wait_frame(VkCtx* ctx, uint64_t v) {
    VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wi.semaphoreCount = 1;
    wi.pSemaphores = &ctx->timeline;
    wi.pValues = &v;
    VkResult r = vkWaitSemaphores(ctx->device, &wi, (uint64_t)RE_GPU_WAIT_S * 1000000000ull);
    if (r == VK_TIMEOUT) {
        re_log(RE_LOG_ERROR, "A GPU nao terminou o quadro %llu em %d s", (unsigned long long)v, RE_GPU_WAIT_S);
        r = VK_ERROR_DEVICE_LOST;
        g_wait_timed_out = true;
    }
    vk_check(r, r == VK_ERROR_DEVICE_LOST ? "vkWaitSemaphores (quadro da GPU)" : "vkWaitSemaphores");
}

// (A9.7c) async compute
VkCommandBuffer vkctx_cmd(VkCtx* ctx) { return ctx->split ? ctx->cmd2[ctx->frame] : ctx->cmd[ctx->frame]; }
VkCommandBuffer vkctx_async_begin(VkCtx* ctx) {
    if (!ctx->async_ok || ctx->split_wait) return VK_NULL_HANDLE; // (one async batch a frame)
    uint32_t f = ctx->frame;
    vk_check(vkResetCommandPool(ctx->device, ctx->async_pool[f], 0), "vkResetCommandPool(async)"); // (its last use waited: the slot's frame)
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(ctx->async_cmd[f], &bi), "vkBeginCommandBuffer(async)");
    return ctx->async_cmd[f];
}
void vkctx_async_submit(VkCtx* ctx, VkPipelineStageFlags2 consumer_stages) {
    uint32_t f = ctx->frame;
    vk_check(vkEndCommandBuffer(ctx->async_cmd[f]), "vkEndCommandBuffer(async)");
    // after the frame before on the graphics queue (it read what this writes), signalling the next async value
    VkSemaphoreSubmitInfo w = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO }, s = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    w.semaphore = ctx->timeline, w.value = ctx->frame_number, w.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    s.semaphore = ctx->async_timeline, s.value = ++ctx->async_value, s.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = ctx->async_cmd[f];
    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.waitSemaphoreInfoCount = ctx->frame_number ? 1 : 0, si.pWaitSemaphoreInfos = &w;
    si.commandBufferInfoCount = 1, si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1, si.pSignalSemaphoreInfos = &s;
    vk_check(vkQueueSubmit2(ctx->async_queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit2(async)");
    ctx->split_wait = ctx->async_value, ctx->split_stage = consumer_stages;
}
VkCommandBuffer vkctx_split(VkCtx* ctx) {
    uint32_t f = ctx->frame;
    if (ctx->split) return ctx->cmd2[f];
    vk_check(vkEndCommandBuffer(ctx->cmd[f]), "vkEndCommandBuffer(split)");
    VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = ctx->cmd[f];
    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.commandBufferInfoCount = 1, si.pCommandBufferInfos = &cbi;
    vk_check(vkQueueSubmit2(ctx->queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit2(split)"); // (no wait: the swapchain comes later)
    ctx->split = true;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(ctx->cmd2[f], &bi), "vkBeginCommandBuffer(split)");
    return ctx->cmd2[f];
}

void vkctx_wait_submitted(VkCtx* ctx) {
    if (!ctx->serialize_frames || !ctx->frame_number) return;
    wait_frame(ctx, ctx->frame_number);
}

static VkCommandBuffer begin_frame(VkCtx* ctx, bool offscreen);
VkCommandBuffer vkctx_begin_frame(VkCtx* ctx) { return begin_frame(ctx, false); }
VkCommandBuffer vkctx_begin_frame_offscreen(VkCtx* ctx) { return begin_frame(ctx, true); }
static VkCommandBuffer begin_frame(VkCtx* ctx, bool offscreen) {
    ctx->offscreen = offscreen;
    if (!offscreen && (ctx->swapchain_dirty || !ctx->swapchain)) {
        if (!vkctx_recreate_swapchain(ctx)) return VK_NULL_HANDLE;
    }
    uint32_t f = ctx->frame;
    // The frame that last used this slot: frame_number + 1 - RE_FRAMES_IN_FLIGHT on the timeline
    if (ctx->frame_number >= RE_FRAMES_IN_FLIGHT) {
        wait_frame(ctx, ctx->frame_number + 1 - RE_FRAMES_IN_FLIGHT);
    }
    if (ctx->timestamps && ctx->timestamps_valid[f]) { // that frame is done: its GPU time is ready
        uint64_t t[2];
        if (vkGetQueryPoolResults(ctx->device, ctx->timestamps, f * 2, 2, sizeof t, t, sizeof t[0],
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            ctx->gpu_ms = (double)(t[1] - t[0]) * ctx->timestamp_ms;
    }

    if (!offscreen) {
        VkResult r = vkAcquireNextImageKHR(ctx->device, ctx->swapchain, UINT64_MAX, ctx->image_ready[f], VK_NULL_HANDLE,
                                           &ctx->image_index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            ctx->swapchain_dirty = true;
            return VK_NULL_HANDLE;
        }
        if (r == VK_SUBOPTIMAL_KHR) ctx->swapchain_dirty = true;
        else vk_check(r, "vkAcquireNextImageKHR");
    }

    vk_check(vkResetCommandPool(ctx->device, ctx->cmd_pool[f], 0), "vkResetCommandPool");
    ctx->split = false, ctx->split_wait = 0; // (A9.7c)
    VkCommandBuffer cmd = ctx->cmd[f];
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
    if (ctx->timestamps) {
        vkCmdResetQueryPool(cmd, ctx->timestamps, f * 2, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, ctx->timestamps, f * 2);
    }
    marks_begin_frame(ctx);
    return cmd;
}

static void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                          VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

void vkctx_begin_rendering(VkCtx* ctx) {
    VkCommandBuffer cmd = vkctx_cmd(ctx);
    // The acquire semaphore is waited at COLOR_ATTACHMENT_OUTPUT: the layout change chains after it
    image_barrier(cmd, ctx->images[ctx->image_index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView = ctx->views[ctx->image_index];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color.float32[0] = ctx->clear_color[0];
    color.clearValue.color.float32[1] = ctx->clear_color[1];
    color.clearValue.color.float32[2] = ctx->clear_color[2];
    color.clearValue.color.float32[3] = 1.0f;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = ctx->extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
}

void vkctx_begin_rendering_on(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h, VkImageLayout from) {
    image_barrier(cmd, image, from, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView = view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = w, ri.renderArea.extent.height = h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
}

void vkctx_end_rendering_on(VkCommandBuffer cmd, VkImage image, VkImageLayout to) {
    vkCmdEndRendering(cmd);
    image_barrier(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, to, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
}

void vkctx_mirror_eyes(VkCtx* ctx, VkImage left, VkImage right, uint32_t w, uint32_t h, VkImageLayout layout) {
    VkCommandBuffer cmd = vkctx_cmd(ctx);
    VkImage win = ctx->images[ctx->image_index];
    image_barrier(cmd, win, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                  VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue black = { { 0, 0, 0, 1 } };
    VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, win, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
    image_barrier(cmd, win, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    // each eye into its half, its shape kept (letterboxed)
    float half = (float)ctx->extent.width * 0.5f, H = (float)ctx->extent.height;
    float s = fminf(half / (float)w, H / (float)h);
    int32_t dw = (int32_t)((float)w * s), dh = (int32_t)((float)h * s), oy = (int32_t)((H - (float)dh) * 0.5f);
    VkImage eyes[2] = { left, right };
    for (int e = 0; e < 2; e++) {
        if (!eyes[e]) continue;
        int32_t ox = (int32_t)(half * (float)e + (half - (float)dw) * 0.5f);
        VkImageBlit b = { 0 };
        b.srcSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        b.dstSubresource = b.srcSubresource;
        b.srcOffsets[1] = (VkOffset3D){ (int32_t)w, (int32_t)h, 1 };
        b.dstOffsets[0] = (VkOffset3D){ ox, oy, 0 };
        b.dstOffsets[1] = (VkOffset3D){ ox + dw, oy + dh, 1 };
        vkCmdBlitImage(cmd, eyes[e], layout, win, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    }
    image_barrier(cmd, win, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

void vkctx_mirror_crop(VkCtx* ctx, VkImage img, uint32_t w, uint32_t h, VkImageLayout layout, float zoom) {
    VkCommandBuffer cmd = vkctx_cmd(ctx);
    VkImage win = ctx->images[ctx->image_index];
    image_barrier(cmd, win, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                  VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    float W = (float)ctx->extent.width, H = (float)ctx->extent.height, a = W / H;
    float cw = (float)w, ch = cw / a;
    if (ch > (float)h) ch = (float)h, cw = ch * a;
    if (zoom > 1.0f) cw /= zoom, ch /= zoom;
    int32_t x0 = (int32_t)(((float)w - cw) * 0.5f), y0 = (int32_t)(((float)h - ch) * 0.5f);
    VkImageBlit b = { 0 };
    b.srcSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    b.dstSubresource = b.srcSubresource;
    b.srcOffsets[0] = (VkOffset3D){ x0, y0, 0 };
    b.srcOffsets[1] = (VkOffset3D){ x0 + (int32_t)cw, y0 + (int32_t)ch, 1 };
    b.dstOffsets[1] = (VkOffset3D){ (int32_t)ctx->extent.width, (int32_t)ctx->extent.height, 1 };
    vkCmdBlitImage(cmd, img, layout, win, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    image_barrier(cmd, win, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

void vkctx_resume_rendering(VkCtx* ctx) {
    VkCommandBuffer cmd = vkctx_cmd(ctx);
    VkRenderingAttachmentInfo color = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView = ctx->views[ctx->image_index];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent = ctx->extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
}

void vkctx_request_grab(VkCtx* ctx, const float rect[4]) {
    memcpy(ctx->grab_rect, rect, sizeof ctx->grab_rect);
    ctx->grab_req = true, ctx->grab_ready = false;
}

void vkctx_request_shot(VkCtx* ctx, const char* png_path) { snprintf(ctx->shot_path, sizeof ctx->shot_path, "%s", png_path ? png_path : ""); }

static uint32_t png_crc(const uint8_t* p, size_t n, uint32_t c) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}
static void png_chunk(FILE* f, const char* type, const uint8_t* data, uint32_t n) {
    uint8_t be[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    fwrite(be, 1, 4, f), fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t c = png_crc((const uint8_t*)type, 4, 0);
    c = png_crc(data, n, c);
    uint8_t cb[4] = { (uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
    fwrite(cb, 1, 4, f);
}
// RGB rows (filter 0) deflated into a PNG
bool vkctx_png_write_rgb(const char* path, const uint8_t* rgb, uint32_t w, uint32_t h) {
    size_t row = (size_t)w * 3 + 1;
    uint8_t* raw = malloc(row * h);
    if (!raw) return false;
    for (uint32_t y = 0; y < h; y++) raw[row * y] = 0, memcpy(&raw[row * y + 1], &rgb[(size_t)w * 3 * y], (size_t)w * 3);
    size_t zn = 0;
    uint8_t* z = re_deflate(raw, row * h, &zn);
    free(raw);
    FILE* f = z ? re_fopen(path, "wb") : NULL;
    if (!f) {
        free(z);
        return false;
    }
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    uint8_t ihdr[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                         (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0 };
    fwrite(sig, 1, 8, f);
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)zn);
    png_chunk(f, "IEND", NULL, 0);
    free(z);
    return fclose(f) == 0;
}

void vkctx_end_frame(VkCtx* ctx) {
    uint32_t f = ctx->frame;
    VkCommandBuffer cmd = vkctx_cmd(ctx);
    if (ctx->offscreen) { // (Q25) a frame of its own images: submitted on the timeline, nothing presented
        if (ctx->timestamps) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ctx->timestamps, f * 2 + 1);
        gpu_mark(cmd, "fim do quadro");
        gpu_mark(cmd, "quadro terminado");
        vk_check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        VkSemaphoreSubmitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO }, sig = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        if (ctx->split_wait) { // (A9.7c) the async compute's work, where the frame's rest uses it
            wait.semaphore = ctx->async_timeline, wait.value = ctx->split_wait;
            wait.stageMask = ctx->split_stage ? ctx->split_stage : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
        sig.semaphore = ctx->timeline, sig.value = ctx->frame_number + 1, sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cbi.commandBuffer = cmd;
        VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        si.waitSemaphoreInfoCount = ctx->split_wait ? 1 : 0, si.pWaitSemaphoreInfos = &wait;
        si.commandBufferInfoCount = 1, si.pCommandBufferInfos = &cbi;
        si.signalSemaphoreInfoCount = 1, si.pSignalSemaphoreInfos = &sig;
        vk_check(vkQueueSubmit2(ctx->queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit2");
        ctx->frame_number++;
        ctx->timestamps_valid[f] = ctx->timestamps != VK_NULL_HANDLE;
        ctx->frame = (ctx->frame + 1) % RE_FRAMES_IN_FLIGHT;
        ctx->offscreen = false;
        return;
    }
    vkCmdEndRendering(cmd);
    // a screenshot: the image copied out before it goes to present
    VkBuffer shot_buf = VK_NULL_HANDLE;
    VkDeviceMemory shot_mem = VK_NULL_HANDLE;
    uint32_t sw = ctx->extent.width, sh = ctx->extent.height;
    double thumb_t0 = ctx->thumb_req ? re_time_seconds() : 0.0;
    if (ctx->shot_path[0] || ctx->thumb_req || ctx->grab_req) { // (and a save's picture: save_progress.h; a movie's frame: B9)
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = (VkDeviceSize)sw * sh * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(ctx->device, &bi, NULL, &shot_buf) == VK_SUCCESS) {
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(ctx->device, shot_buf, &mr);
            VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = UINT32_MAX;
            { // (cached host memory first: the CPU reads it back; the uncached kind reads at a crawl, ~1 s for 2560x1440)
                VkPhysicalDeviceMemoryProperties mp;
                vkGetPhysicalDeviceMemoryProperties(ctx->gpu, &mp);
                const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
                for (uint32_t i = 0; i < mp.memoryTypeCount && ai.memoryTypeIndex == UINT32_MAX; i++)
                    if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) ai.memoryTypeIndex = i;
            }
            if (ai.memoryTypeIndex == UINT32_MAX)
                ai.memoryTypeIndex = vkctx_find_memory(ctx, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (vkAllocateMemory(ctx->device, &ai, NULL, &shot_mem) != VK_SUCCESS || vkBindBufferMemory(ctx->device, shot_buf, shot_mem, 0) != VK_SUCCESS) {
                vkDestroyBuffer(ctx->device, shot_buf, NULL), shot_buf = VK_NULL_HANDLE;
                if (shot_mem) vkFreeMemory(ctx->device, shot_mem, NULL), shot_mem = VK_NULL_HANDLE;
            }
        }
    }
    if (shot_buf) {
        image_barrier(cmd, ctx->images[ctx->image_index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy rg = { 0 };
        rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rg.imageSubresource.layerCount = 1;
        rg.imageExtent.width = sw, rg.imageExtent.height = sh, rg.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cmd, ctx->images[ctx->image_index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot_buf, 1, &rg);
        image_barrier(cmd, ctx->images[ctx->image_index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_NONE, 0);
    } else
        image_barrier(cmd, ctx->images[ctx->image_index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    if (ctx->timestamps) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ctx->timestamps, f * 2 + 1);
    gpu_mark(cmd, "fim do quadro"); // (its "done" write below it says the whole frame finished)
    gpu_mark(cmd, "quadro terminado");
    vk_check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkSemaphoreSubmitInfo waits[2] = { { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO }, { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO } };
    waits[0].semaphore = ctx->image_ready[f];
    waits[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (ctx->split_wait) { // (A9.7c) the async compute's work, where the frame's rest uses it
        waits[1].semaphore = ctx->async_timeline, waits[1].value = ctx->split_wait;
        waits[1].stageMask = ctx->split_stage ? ctx->split_stage : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    VkSemaphoreSubmitInfo signal[2] = { { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO },
                                        { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO } };
    signal[0].semaphore = ctx->render_done[ctx->image_index];
    signal[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signal[1].semaphore = ctx->timeline;
    signal[1].value = ctx->frame_number + 1;
    signal[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.waitSemaphoreInfoCount = ctx->split_wait ? 2 : 1;
    si.pWaitSemaphoreInfos = waits;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 2;
    si.pSignalSemaphoreInfos = signal;
    vk_check(vkQueueSubmit2(ctx->queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit2");
    ctx->frame_number++;
    if (shot_buf) { // (a rare request: the GPU waited for, the pixels written as RGB)
        double wait0 = re_time_seconds();
        vkQueueWaitIdle(ctx->queue);
        double wait_ms = (re_time_seconds() - wait0) * 1000.0;
        void* p = NULL;
        bool png = ctx->shot_path[0] != 0; // (a save's picture alone: sampled where the frame lies, no full copy)
        uint8_t* rgb = png ? malloc((size_t)sw * sh * 3) : NULL;
        ctx->shot_ok = false;
        if ((rgb || !png) && vkMapMemory(ctx->device, shot_mem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
            const uint8_t* s = p;
            bool bgr = ctx->color_format == VK_FORMAT_B8G8R8A8_UNORM || ctx->color_format == VK_FORMAT_B8G8R8A8_SRGB;
            const int r0 = bgr ? 2 : 0, b0 = bgr ? 0 : 2;
            for (size_t i = 0; png && i < (size_t)sw * sh; i++)
                rgb[i * 3] = s[i * 4 + r0], rgb[i * 3 + 1] = s[i * 4 + 1], rgb[i * 3 + 2] = s[i * 4 + b0];
            if (png) {
                ctx->shot_ok = vkctx_png_write_rgb(ctx->shot_path, rgb, sw, sh);
                printf("[RealEngine] Screenshot %s: %s (%ux%u)\n", ctx->shot_ok ? "written" : "FAILED", ctx->shot_path, sw, sh);
            }
            if (ctx->thumb_req) { // the rect cut to 16:9 about its center, each pixel the mean of up to 4 x 4 samples
                float x0 = ctx->thumb_rect[0], y0 = ctx->thumb_rect[1], x1 = ctx->thumb_rect[2], y1 = ctx->thumb_rect[3];
                if (!(x1 > x0 + 8.0f) || !(y1 > y0 + 8.0f)) x0 = 0.0f, y0 = 0.0f, x1 = (float)sw, y1 = (float)sh;
                x0 = fmaxf(x0, 0.0f), y0 = fmaxf(y0, 0.0f), x1 = fminf(x1, (float)sw), y1 = fminf(y1, (float)sh);
                float rw = x1 - x0, rh = y1 - y0;
                if (rw > rh * 16.0f / 9.0f) x0 += (rw - rh * 16.0f / 9.0f) * 0.5f, rw = rh * 16.0f / 9.0f;
                else y0 += (rh - rw * 9.0f / 16.0f) * 0.5f, rh = rw * 9.0f / 16.0f;
                const uint32_t tw = 192, th = 108;
                float fx = rw / (float)tw, fy = rh / (float)th;
                int nx = fx >= 4.0f ? 4 : fx >= 2.0f ? 2 : 1, ny = fy >= 4.0f ? 4 : fy >= 2.0f ? 2 : 1;
                for (uint32_t y = 0; y < th; y++)
                    for (uint32_t x = 0; x < tw; x++) {
                        uint32_t acc[3] = { 0, 0, 0 };
                        for (int j = 0; j < ny; j++)
                            for (int i = 0; i < nx; i++) {
                                uint32_t sx = (uint32_t)(x0 + ((float)x + ((float)i + 0.5f) / (float)nx) * fx);
                                uint32_t sy2 = (uint32_t)(y0 + ((float)y + ((float)j + 0.5f) / (float)ny) * fy);
                                if (sx >= sw) sx = sw - 1;
                                if (sy2 >= sh) sy2 = sh - 1;
                                const uint8_t* q = s + ((size_t)sy2 * sw + sx) * 4;
                                acc[0] += q[r0], acc[1] += q[1], acc[2] += q[b0];
                            }
                        uint8_t* o = ctx->thumb_px + ((size_t)y * tw + x) * 3;
                        for (int c = 0; c < 3; c++) o[c] = (uint8_t)(acc[c] / (uint32_t)(nx * ny));
                    }
                ctx->thumb_w = (uint16_t)tw, ctx->thumb_h = (uint16_t)th, ctx->thumb_ready = true;
            }
            if (ctx->grab_req) { // (B9: the rect, whole pixels, as RGB rows)
                uint32_t gx = (uint32_t)fmaxf(ctx->grab_rect[0], 0.0f), gy = (uint32_t)fmaxf(ctx->grab_rect[1], 0.0f);
                uint32_t gw = (uint32_t)ctx->grab_rect[2], gh = (uint32_t)ctx->grab_rect[3];
                if (!gw || gx + gw > sw) gx = 0, gw = sw;
                if (!gh || gy + gh > sh) gy = 0, gh = sh;
                size_t need = (size_t)gw * gh * 3;
                if (need > ctx->grab_cap) {
                    uint8_t* q = realloc(ctx->grab_px, need);
                    if (q) ctx->grab_px = q, ctx->grab_cap = (uint32_t)need;
                }
                if (need <= ctx->grab_cap) {
                    for (uint32_t y = 0; y < gh; y++)
                        for (uint32_t x = 0; x < gw; x++) {
                            const uint8_t* q = s + ((size_t)(gy + y) * sw + gx + x) * 4;
                            uint8_t* o = ctx->grab_px + ((size_t)y * gw + x) * 3;
                            o[0] = q[r0], o[1] = q[1], o[2] = q[b0];
                        }
                    ctx->grab_w = gw, ctx->grab_h = gh, ctx->grab_ready = true;
                }
                ctx->grab_req = false;
            }
            vkUnmapMemory(ctx->device, shot_mem);
        }
        free(rgb);
        vkDestroyBuffer(ctx->device, shot_buf, NULL), vkFreeMemory(ctx->device, shot_mem, NULL);
        ctx->shot_path[0] = 0;
        if (ctx->thumb_req) {
            ctx->thumb_ms = (re_time_seconds() - thumb_t0) * 1000.0;
            printf("[RealEngine] Save picture: %.2f ms (the frame's GPU waited for %.2f ms, the copy and scale %.2f ms)\n", ctx->thumb_ms, wait_ms, ctx->thumb_ms - wait_ms);
        }
        ctx->thumb_req = false;
    }
    ctx->timestamps_valid[f] = ctx->timestamps != VK_NULL_HANDLE;

    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &ctx->render_done[ctx->image_index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &ctx->swapchain;
    pi.pImageIndices = &ctx->image_index;
    VkResult r = vkQueuePresentKHR(ctx->queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) ctx->swapchain_dirty = true;
    else vk_check(r, "vkQueuePresentKHR");

    ctx->frame = (ctx->frame + 1) % RE_FRAMES_IN_FLIGHT;
}
