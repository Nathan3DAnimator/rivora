// The Android game (notas-dev/android.md): Android's libvulkan exports the Vulkan 1.1 core only. The 1.2 and 1.3
// functions the engine calls are renamed by src/re_compat.h (vkCmdBeginRendering -> re_vkCmdBeginRendering ...) and
// defined here, forwarding to the device's own (vkGetDeviceProcAddr after vkCreateDevice: vkctx.c calls re_vk_android_load),
// the KHR name as a fallback (a 1.1 driver with the extensions).
#include <vulkan/vulkan.h>

static PFN_vkCmdBeginRendering p_begin_rendering;
static PFN_vkCmdEndRendering p_end_rendering;
static PFN_vkCmdPipelineBarrier2 p_barrier2;
static PFN_vkCmdWriteTimestamp2 p_timestamp2;
static PFN_vkQueueSubmit2 p_submit2;
static PFN_vkWaitSemaphores p_wait_semaphores;
static PFN_vkCmdDrawIndexedIndirectCount p_draw_count;
static PFN_vkGetBufferDeviceAddress p_bda;

static PFN_vkVoidFunction get(VkDevice d, const char* core, const char* khr) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(d, core);
    return f ? f : vkGetDeviceProcAddr(d, khr);
}

void re_vk_android_load(VkDevice d) {
    p_begin_rendering = (PFN_vkCmdBeginRendering)get(d, "vkCmdBeginRendering", "vkCmdBeginRenderingKHR");
    p_end_rendering = (PFN_vkCmdEndRendering)get(d, "vkCmdEndRendering", "vkCmdEndRenderingKHR");
    p_barrier2 = (PFN_vkCmdPipelineBarrier2)get(d, "vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR");
    p_timestamp2 = (PFN_vkCmdWriteTimestamp2)get(d, "vkCmdWriteTimestamp2", "vkCmdWriteTimestamp2KHR");
    p_submit2 = (PFN_vkQueueSubmit2)get(d, "vkQueueSubmit2", "vkQueueSubmit2KHR");
    p_wait_semaphores = (PFN_vkWaitSemaphores)get(d, "vkWaitSemaphores", "vkWaitSemaphoresKHR");
    p_draw_count = (PFN_vkCmdDrawIndexedIndirectCount)get(d, "vkCmdDrawIndexedIndirectCount", "vkCmdDrawIndexedIndirectCountKHR");
    p_bda = (PFN_vkGetBufferDeviceAddress)get(d, "vkGetBufferDeviceAddress", "vkGetBufferDeviceAddressKHR");
}
// (whether the device has them all: the engine needs Vulkan 1.3's dynamic rendering and synchronization2)
int re_vk_android_ready(void) {
    return p_begin_rendering && p_end_rendering && p_barrier2 && p_submit2 && p_wait_semaphores && p_bda && p_draw_count;
}

void re_vkCmdBeginRendering(VkCommandBuffer c, const VkRenderingInfo* i) { p_begin_rendering(c, i); }
void re_vkCmdEndRendering(VkCommandBuffer c) { p_end_rendering(c); }
void re_vkCmdPipelineBarrier2(VkCommandBuffer c, const VkDependencyInfo* d) { p_barrier2(c, d); }
void re_vkCmdWriteTimestamp2(VkCommandBuffer c, VkPipelineStageFlags2 s, VkQueryPool q, uint32_t n) {
    if (p_timestamp2) p_timestamp2(c, s, q, n);
}
VkResult re_vkQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) { return p_submit2(q, n, s, f); }
VkResult re_vkWaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo* w, uint64_t t) { return p_wait_semaphores(d, w, t); }
void re_vkCmdDrawIndexedIndirectCount(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkBuffer cb, VkDeviceSize co, uint32_t m, uint32_t s) {
    p_draw_count(c, b, o, cb, co, m, s);
}
VkDeviceAddress re_vkGetBufferDeviceAddress(VkDevice d, const VkBufferDeviceAddressInfo* i) { return p_bda ? p_bda(d, i) : 0; }
