// The Android game: what the Android layer (glfw_android.c, platform_android.c) offers the rest of the app.
// Only compiled on Android (__ANDROID__); the PC never includes this header.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __ANDROID__
#include <jni.h>

typedef enum PlatformWheel { PLATFORM_WHEEL_MOUSE = 0, PLATFORM_WHEEL_TOUCHPAD } PlatformWheel;

struct android_app;
struct android_app* re_android_app(void);
// The JNI environment of the app thread (attached once in android_main) and the activity object (GameActivity).
JNIEnv* re_android_env(void);
jobject re_android_activity(void);
// Folders: internal files (settings, extracted data, crash logs), the shared documents folder the user sees over USB
// (Android/data/the game's package/files/Projetos) and the cache (files picked from other apps are copied here).
const char* re_android_files_dir(void);
const char* re_android_documents_dir(void);
const char* re_android_cache_dir(void);
// The screen's density as a scale (dpi / 160) and the diagonal in inches (phone or tablet defaults).
float re_android_density(void);
float re_android_screen_inches(void);

// The Vulkan surface lives with the window: the app goes to the background (or a file picker covers it) and Android
// destroys the window; the swapchain and the surface must go first, and come back with the new window.
void re_android_set_surface_callbacks(void (*lost)(void* user), void (*gained)(void* user), void* user);
// Running smoothly (src/mobile.c): the window's buffers at a share of the screen (the compositor upscales them; the input
// comes in the buffers' pixels), the frame rate told to the display, the system's low-memory warnings so far
void re_android_set_content_scale(float scale);
void re_android_set_frame_rate(float fps);
int re_android_low_memory_count(void);
// The activity paused (true) or resumed (false): ARCore gives the camera back in the background (ar_arcore.c). NULL: none.
void re_android_set_pause_callback(void (*cb)(bool paused));

// Waits (pumping the activity's lifecycle, dropping input) until *done turns nonzero: a picker or another activity on
// top of the app. The UI does not draw meanwhile.
void re_android_wait(volatile int* done);

// Calls a void method of the activity with a signature, e.g. re_android_call_void("showKeyboard", "(Z)V", JNI_TRUE).
void re_android_call_void(const char* name, const char* sig, ...);
// The same for a method returning a String: the UTF-8 copy in out (empty on failure).
bool re_android_call_string(const char* name, const char* sig, char* out, size_t cap, ...);

// HTTP through Java's HttpURLConnection (net_http.c on Android; any thread): the status (0: no answer, err says why), the
// body malloc'd into body_out (NUL-terminated) or written to out_path.
#include <stdint.h>
int re_android_http(const char* method, const char* url, const char* headers, const void* body, size_t body_len,
                    const char* out_path, int timeout_ms, bool (*progress)(void*, uint64_t, uint64_t), void* user,
                    uint8_t** body_out, size_t* len_out, char* err, size_t err_cap);
// The JNI environment of the calling thread, attached when it was not (pass *attached to re_android_thread_env_end).
JNIEnv* re_android_thread_env(bool* attached);
void re_android_thread_env_end(bool attached);
// ECDSA P-256 over a SHA-256 (the updater's package signature, tl_crypto.c): Java's Signature (any thread).
// The opening animation (intro.c) in a window over the app (GameActivity.introShow/introFrame/introHide; any thread).
// introCaption: a stage's name as coverage (w x h, capitals, letter-spaced), false when Java could not draw it.
// The system's package installer for a downloaded APK under the cache (the updater; the main thread).
// A beta started with "--es tlender_crash_test <kind>" crashes on purpose (TLENDER_CRASH_TEST, platform_android.c).
void re_android_read_test_extras(void);
#endif
