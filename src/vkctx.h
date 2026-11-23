// Vulkan 1.3 context: instance, device, swapchain and the frames in flight.
// Adapted from the Tlender's vkctx (Vulkan 1.2, render pass + fences): here dynamic rendering (no render pass nor
// framebuffers), synchronization2 (vkCmdPipelineBarrier2, vkQueueSubmit2) and one timeline semaphore that counts the
// submitted frames (no fences). Descriptor indexing and buffer device address are enabled for the bindless renderer.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

struct GLFWwindow;

#define RE_FRAMES_IN_FLIGHT 2
#define RE_MAX_SWAP_IMAGES 8

typedef struct VkCtx {
    struct GLFWwindow* window;
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkSurfaceKHR surface;
    VkPhysicalDevice gpu;
    VkPhysicalDeviceProperties gpu_props;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;

    VkSwapchainKHR swapchain;
    VkFormat color_format;
    // (A9.4) HDR10 output: asked for (want_hdr, then vkctx_recreate_swapchain), possible (the instance has
    // VK_EXT_swapchain_colorspace), in use (the swapchain is A2B10G10R10 in HDR10 ST.2084: the composite writes PQ)
    bool want_hdr, hdr_possible, hdr;
    VkColorSpaceKHR color_space;
    VkPresentModeKHR present_mode;
    VkExtent2D extent;
    uint32_t min_image_count;
    uint32_t image_count;
    VkImage images[RE_MAX_SWAP_IMAGES];
    VkImageView views[RE_MAX_SWAP_IMAGES];
    VkSemaphore render_done[RE_MAX_SWAP_IMAGES]; // binary, per swapchain image (present waits on it)

    VkCommandPool cmd_pool[RE_FRAMES_IN_FLIGHT];
    VkCommandPool once_pool; // gpu_begin_once's (render.c)
    VkCommandBuffer cmd[RE_FRAMES_IN_FLIGHT];
    VkSemaphore image_ready[RE_FRAMES_IN_FLIGHT]; // binary (acquire)
    VkSemaphore timeline;   // value n = frame n done on the GPU
    // (A9.7c) async compute: a queue of a compute-only family (when the GPU has one), its pools and command buffers and
    // its timeline; a frame split in two submissions (vkctx_split) so only its second part waits for the compute work
    VkQueue async_queue;
    uint32_t async_family;
    bool async_ok;
    VkCommandPool async_pool[RE_FRAMES_IN_FLIGHT];
    VkCommandBuffer async_cmd[RE_FRAMES_IN_FLIGHT];
    VkSemaphore async_timeline;
    uint64_t async_value;
    VkCommandBuffer cmd2[RE_FRAMES_IN_FLIGHT]; // (the frame's second part)
    bool split;                 // this frame was split: the rest records into cmd2
    uint64_t split_wait;        // the async value its last submission waits for (0: none)
    VkPipelineStageFlags2 split_stage;
    uint64_t frame_number;  // frames submitted so far (the value the last one signals)
    uint32_t frame;         // frame-in-flight slot
    uint32_t image_index;

    // GPU timestamps: two per frame slot (start of the command buffer, end of the frame)
    VkQueryPool timestamps;
    double timestamp_ms;    // ms per tick (timestampPeriod / 1e6)
    double gpu_ms;          // the GPU time of the last finished frame
    bool timestamps_valid[RE_FRAMES_IN_FLIGHT];

    bool ray_query; // VK_KHR_acceleration_structure + VK_KHR_ray_query enabled (the GI, the path tracer)
    bool omm;       // VK_EXT_opacity_micromap enabled (gi_omm.c: the Masked meshes' BLAS)
    bool clas;      // VK_NV_cluster_acceleration_structure enabled (rt_clusters.c, A8: the static BLAS from clusters)
    uint32_t clas_max_tris, clas_max_verts, clas_max_geometry, clas_align, clas_blas_align, clas_scratch_align;
    // VK_KHR_fragment_shading_rate with a rate image (render.c's VRS, A9.7b): its texel (px) the GPU takes; 0: none
    bool vrs;
    uint32_t vrs_texel;
    bool large_points; // points over a pixel (the particles' small sprites: one vertex each)
    bool frag_stores;  // (Q7) fragmentStoresAndAtomics: the asset previews' AO view mode (render_preview.c)
    bool bc_textures;  // BC1-BC7 sampled (the PCs; most phones' GPUs not: the materials stay RGBA8, notas-dev/android.md)
    bool unused_attachments; // VK_EXT_dynamic_rendering_unused_attachments: an attachment without an image (render.c's split)
    bool shader_stats; // --shader-stats: VK_KHR_pipeline_executable_properties, the main pass's registers (render.c)
    bool serialize_frames; // vkctx_wait_submitted waits: NVIDIA drivers before 617 (their background compiler crash)
    bool gpu_markers;  // VK_AMD_buffer_marker: gpu_mark's passes (where a lost GPU stopped, in the crash report)
    bool device_fault; // VK_EXT_device_fault: the driver's reason and faulting address after a device lost

    // The pipelines: one cache kept on disk (vkctx_pipeline_cache_save), and what creating them cost (the first run of a
    // program the driver has never seen compiles every shader: render.c counts it)
    VkPipelineCache pipeline_cache;
    size_t pipeline_cache_loaded; // bytes read at start (0: started cold)
    size_t pipeline_cache_saved;  // bytes on disk now (read or last written)
    bool pipeline_cache_off;      // REALENGINE_PIPELINE_CACHE=off: the file neither read nor written (a cold start measured)
    uint32_t pipelines;
    double pipelines_ms;

    bool swapchain_dirty;
    bool offscreen; // (Q25) this frame draws into images of its own (a VR eye): no swapchain image, nothing presented
    bool vsync;             // FIFO; off: MAILBOX (or IMMEDIATE) so the frame rate passes the monitor's
    float clear_color[3];
    char shot_path[520]; // a screenshot of the next frame into this PNG (vkctx_request_shot; "" when none)
    bool shot_ok;        // the last one was written
    // a save's picture (save_progress.h): the next frame's thumb_rect (window pixels; all 0: the whole window), cut to
    // 16:9 and scaled down into thumb_px (RGB, thumb_w x thumb_h); thumb_ready once it is there
    bool thumb_req, thumb_ready;
    float thumb_rect[4];
    uint16_t thumb_w, thumb_h;
    double thumb_ms;     // the copy (the GPU waited for) and the scale
    uint8_t thumb_px[192 * 108 * 3];
    // (B9) the Movie Render Queue's grab: the next frame's grab_rect (window pixels) as RGB into grab_px (grab_w x grab_h,
    // grown as needed); grab_ready once it is there (the caller clears it)
    bool grab_req, grab_ready;
    float grab_rect[4];
    uint8_t* grab_px;
    uint32_t grab_w, grab_h, grab_cap;
} VkCtx;

void vk_check(VkResult result, const char* what);
const char* vk_result_name(VkResult r);
bool vkctx_init(VkCtx* ctx, struct GLFWwindow* window, bool validation);
extern int g_vk_validation_errors; // errors the validation layer reported (with --validation)
// VR (xr.c): the extensions the OpenXR runtime needs, set before vkctx_init; the GPU it drives (NULL: the best one)
extern const char* g_vk_extra_instance_exts[16];
extern uint32_t g_vk_extra_ninst;
extern const char* g_vk_extra_device_exts[24];
extern uint32_t g_vk_extra_ndev;
extern VkPhysicalDevice (*g_vk_pick_gpu)(VkInstance instance);
// VR: an eye's image as the target (its layout from UNDEFINED or what it had; cleared), then left as transfer source
void vkctx_begin_rendering_on(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h, VkImageLayout from);
void vkctx_end_rendering_on(VkCommandBuffer cmd, VkImage image, VkImageLayout to);
// VR: the eyes' images side by side into the window's image (left as color attachment), then rendering on it again, loaded
void vkctx_mirror_eyes(VkCtx* ctx, VkImage left, VkImage right, uint32_t w, uint32_t h, VkImageLayout layout);
// (Q25) One image into the whole window, its middle cropped to the window's shape (zoom > 1: a narrower part of it): the
// VR mirror for recording or streaming (one eye, the spectator's view), no black bars, no two eyes side by side
void vkctx_mirror_crop(VkCtx* ctx, VkImage img, uint32_t w, uint32_t h, VkImageLayout layout, float zoom);
void vkctx_resume_rendering(VkCtx* ctx);
void vkctx_shutdown(VkCtx* ctx);
// Writes the pipeline cache (%LOCALAPPDATA%/RealEngine/pipelines.bin, or the file REALENGINE_PIPELINE_CACHE names) when it
// grew: the editor and every packaged game share it (the same shaders), so a game packaged here starts warm
void vkctx_pipeline_cache_save(VkCtx* ctx);
// Rebuilds the swapchain for the current window size. Returns false while minimized.
bool vkctx_recreate_swapchain(VkCtx* ctx);
// (A9.7c) async compute: the frame's command buffer being recorded (a split frame's second part after vkctx_split); a
// command buffer for the compute queue (NULL: none, or already one this frame), submitted after the frame before (it
// signals the async timeline; the frame's last submission waits for it at consumer_stages); the frame so far submitted,
// recording going on in its second part
VkCommandBuffer vkctx_cmd(VkCtx* ctx);
VkCommandBuffer vkctx_async_begin(VkCtx* ctx);
void vkctx_async_submit(VkCtx* ctx, VkPipelineStageFlags2 consumer_stages);
VkCommandBuffer vkctx_split(VkCtx* ctx);
// Waits for the slot's previous frame (timeline), acquires the next image and starts the command buffer. Returns
// VK_NULL_HANDLE when the frame must be skipped; else record work outside rendering, then vkctx_begin_rendering.
VkCommandBuffer vkctx_begin_frame(VkCtx* ctx);
// (Q25) A frame with no window image (the first VR eye; every frame of the Quest, whose window isn't shown): the same
// slot, command buffer and timeline; vkctx_end_frame submits it without presenting (no rendering left open)
VkCommandBuffer vkctx_begin_frame_offscreen(VkCtx* ctx);
// Waits until every frame submitted so far is done on the GPU (main.c: before the UI is recorded, see vkctx.c)
void vkctx_wait_submitted(VkCtx* ctx);
// Moves the swapchain image to color attachment and begins dynamic rendering into it (cleared). The UI draws here; the
// scene will render offscreen into its own targets, shown by the viewport.
void vkctx_begin_rendering(VkCtx* ctx);
// Ends rendering, moves the image to present, submits (signals the timeline) and presents.
void vkctx_end_frame(VkCtx* ctx);
// The window's next frame (the editor's UI and all) into a PNG: copied from the swapchain at the end of that frame
// (one wait for the GPU, then the file). The engine's own screenshot: the tests and the docs see the app, not the screen.
void vkctx_request_shot(VkCtx* ctx, const char* png_path);
void vkctx_request_grab(VkCtx* ctx, const float rect[4]); // (B9) the next frame's rect into grab_px
// RGB rows into a PNG (the screenshots, the animation sheets: asset_viewer.c)
bool vkctx_png_write_rgb(const char* path, const uint8_t* rgb, uint32_t w, uint32_t h);

// GPU crash markers (notas-dev/servicos.md "Onde a GPU parou"): each pass of the frame's command buffer marks its start
// with a name (a string literal: kept by pointer); the GPU writes the pass's number into host memory when it starts it
// and when everything before it is done. After VK_ERROR_DEVICE_LOST the crash report says which passes the GPU had
// started but not finished. A pointer stored on the CPU, two 4-byte writes on the GPU (measured in servicos.md); a
// no-op without VK_AMD_buffer_marker or outside the frame's command buffer.
#define RE_GPU_MARKS 1024 // marks per frame (more are ignored)
void gpu_mark(VkCommandBuffer cmd, const char* name);
// The GPU's time per pass (Unreal's "stat gpu"): while on, each gpu_mark writes a timestamp after the work before it; a
// pass is the time from its mark to the next one. --gpu-passes or REALENGINE_GPU_PASSES=1, the Debug Viewer's GPU Passes.
// The sums since on or the last reset: ms over the frames each pass ran, last_ms the latest frame's
typedef struct GpuPassStat {
    const char* name;
    double ms, last_ms;
    uint32_t frames;
} GpuPassStat;
// A finer pass inside a marked one (the weather's, the main pass's parts): only while the timing is on
void gpu_mark_detail(VkCommandBuffer cmd, const char* name);
void vkctx_pass_timing(bool on);
bool vkctx_pass_timing_on(void);
uint32_t vkctx_pass_stats(const GpuPassStat** out, uint32_t* frames);
void vkctx_pass_stats_reset(void);
// After a device lost: the markers and the driver's fault info as text lines (full: the log; where: one sentence for
// the report's message). Either "" when there is nothing to say.
void vkctx_gpu_diagnosis(char* full, size_t full_cap, char* where, size_t where_cap);
// The marks the GPU wrote for the last submitted frame: the last pass started and the last one with all before it
// done (the test: after vkDeviceWaitIdle both are the end of the frame). False without markers.
bool vkctx_gpu_marks_last(const char** started, const char** finished, uint32_t* count);

uint32_t vkctx_find_memory(VkCtx* ctx, uint32_t type_bits, VkMemoryPropertyFlags props);
