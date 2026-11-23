// The engine's presentation (notas-dev/marca.md): the animated intro (the logo assembling, before the Project Browser
// and before the loading screen) and the animated loading screen (the splash: the project's own image or video, or
// the engine's), drawn with ImGui over a cleared swapchain. The editor and the packaged game both use it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct VkCtx;
struct GLFWwindow;

// The logo and the engine's splash (embedded in the executable), uploaded for ImGui. After ImGui's Vulkan backend.
bool brand_init(struct VkCtx* vk);
void brand_shutdown(struct VkCtx* vk);
// The loading screen's picture: an image (PNG, JPEG...) or a video (MP4, WMV...: Media Foundation), NULL or "" for the
// engine's. False (and the engine's kept) when it can't be read.
bool brand_set_splash(struct VkCtx* vk, const char* path);

// The intro as whole frames until it ends (3.4 s) or a key / click skips it. skippable: false in the tests.
// (2026-11-23) The editor's starts earlier, right after glfwInit, in a transparent window of its own over the desktop
// (brand_intro_start: Windows; background: off the screens, never focused; mon_w > 0: centered on that display), and
// brand_run_intro then only waits for it to end. False: not started (the intro off, or no thread).
bool brand_intro_start(bool background, int mon_x, int mon_y, int mon_w, int mon_h);
void brand_run_intro(struct VkCtx* vk, struct GLFWwindow* window);
// The loading screen: progress 0..1 and what loads now (translated by the caller). brand_splash_frame draws a whole
// frame of it (between the loading's steps, outside the main loop); brand_splash_draw adds it to the current ImGui
// frame. brand_splash_begin starts its clock (the fade in), brand_splash_end fades it out over a few frames, once it
// has been shown BRAND_SPLASH_MIN_S (a project loaded in the editor or the game: never a flash).
#define BRAND_SPLASH_MIN_S 1.5
void brand_splash_begin(void);
void brand_splash_frame(struct VkCtx* vk, float progress, const char* status);
void brand_splash_draw(double t, float progress, const char* status);
void brand_splash_end(struct VkCtx* vk);
void brand_splash_fade(void); // the editor's first frames: the loading screen fading out over them (call before igRender)
bool brand_splash_custom(void); // the project's own picture is in use
// UMG's Image widgets (ui_widget_draw.c): a picture by its "/Content/..." path, loaded on its first use and kept for the
// session (up to 32); its ImGui texture (NULL: none, or it can't be read), its width / height
void* brand_ui_image(const char* content_root, const char* vpath, float* aspect);
// A picture made at run time (a save's: save_progress.h), RGB, by its key; uploaded again when gen changes. Its texture
void* brand_ui_image_rgb(const char* key, const uint8_t* rgb, uint32_t w, uint32_t h, uint32_t gen);
// (Q26) The logo as a white mark with alpha (64 x 64, made once): the menu bar tints it with the theme's accent
void* brand_mark(void);

// Switches (the command line's --no-intro, the tests)
extern bool g_brand_intro_on, g_brand_splash_on;
