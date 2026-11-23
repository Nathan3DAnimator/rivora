// The Android game (copied from the Tlender Mobile): the part of GLFW 3.4 the engine uses, over the NDK's native_app_glue (NativeActivity).
//
// The app keeps one code path for the PC and for Android: main.c, input.c and ImGui's GLFW backend call GLFW as on
// Windows and this file answers from the Android window and input queue:
// - one window (the activity's), sized in pixels; the content scale is the screen density (dpi / 160);
// - a finger is the mouse: a tap clicks, a drag past a few pixels drags with the left button, press and hold clicks with
//   the right button (as Windows does with touch screens). Two or three fingers are a gesture, never a click: their pan
//   and pinch are accumulated for platform_touch_take (the 3D viewport orbits, zooms and pans; the node editors pan and
//   zoom; the panels scroll);
// - a pen (S Pen, the tablets' styluses) draws as the left button with its pressure (platform_pen_pressure), hovers as
//   the mouse moving, and its side button is the middle button (Blender's orbit with a tablet);
// - a mouse and a keyboard (USB, Bluetooth, Samsung DeX) work as on the PC: buttons, wheel, keys and modifiers;
// - the on-screen keyboard is shown while ImGui wants text (platform_text_input) and its text comes through the activity
//   (GameActivity.java's input connection) as GLFW characters.
#include <android/configuration.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "crashlog.h"
#include "platform.h"
#include "re_android.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "RealEngine", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "RealEngine", __VA_ARGS__)

// ---------------------------------------------------------------------------------------------------------------------
// State
struct GLFWwindow {
    void* user;
    GLFWwindowclosefun close_cb;
    GLFWwindowfocusfun focus_cb;
    GLFWcursorposfun cursor_cb;
    GLFWcursorenterfun enter_cb;
    GLFWmousebuttonfun button_cb;
    GLFWscrollfun scroll_cb;
    GLFWkeyfun key_cb;
    GLFWcharfun char_cb;
    GLFWdropfun drop_cb;
    int should_close;
    int cursor_mode;
    int focused;
    double cx, cy;     // the cursor the app sees (the physical point plus the warp offset)
    double off_x, off_y; // glfwSetCursorPos moves this offset: the next drag continues from the warped point
    unsigned char keys[GLFW_KEY_LAST + 1];
    unsigned char buttons[GLFW_MOUSE_BUTTON_LAST + 1];
};

struct GLFWcursor { int shape; };

static struct android_app* g_app;
static JavaVM* g_vm;
static JNIEnv* g_env;
static jobject g_activity;
static GLFWwindow g_window_storage;
static GLFWwindow* g_window;
static GLFWerrorfun g_error_cb;
static double g_time0;
static float g_density = 1.0f;
static float g_inches = 6.0f;
static char g_files_dir[512], g_docs_dir[512], g_cache_dir[512];
static int g_modal; // re_android_wait: input is dropped
static int g_resumed = 1;
static void (*g_pause_cb)(bool paused); // (re_android_set_pause_callback: ARCore's session)
void re_android_set_pause_callback(void (*cb)(bool paused)) { g_pause_cb = cb; }
// The window's buffers against the screen (re_android_set_content_scale: Unreal's r.MobileContentScaleFactor, the
// compositor upscales for free) and the input brought to the buffers' pixels; the low-memory warnings counted
static float g_content_scale = 1.0f, g_in_scale = 1.0f, g_frame_rate;
#define GX(e, i) (AMotionEvent_getX((e), (i)) * g_in_scale)
#define GY(e, i) (AMotionEvent_getY((e), (i)) * g_in_scale)
static volatile int g_low_memory;
static void (*g_surface_lost)(void*);
static void (*g_surface_gained)(void*);
static void* g_surface_user;
static char* g_clipboard;

// Characters and key taps from the Java thread (the on-screen keyboard), drained by glfwPollEvents.
static pthread_mutex_t g_text_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_text[256];
static int g_text_n;
static int g_text_keys[64]; // Android key codes tapped by the input method (backspace, enter)
static int g_text_keys_n;

// ---------------------------------------------------------------------------------------------------------------------
// Pen and touch (platform.h)
static float s_pen_pressure = -1.0f;
static PlatformWheel s_last_wheel = PLATFORM_WHEEL_MOUSE;
static struct {
    float pan_x, pan_y, zoom, cx, cy;
    int fingers;
    bool any;
} s_gesture = { 0, 0, 1.0f, 0, 0, 0, false };

typedef enum TouchState { TOUCH_IDLE, TOUCH_PENDING, TOUCH_DRAG, TOUCH_GESTURE, TOUCH_DONE, TOUCH_HOVER } TouchState;
static struct {
    TouchState state;
    double t0;
    float x0, y0;      // where the first finger went down
    float x, y;        // where it is
    int count;         // fingers down
    float gx, gy, gd;  // the gesture's last centroid and spread
} s_touch;

void platform_input_hook(struct GLFWwindow* window) { (void)window; }
PlatformWheel platform_last_wheel(void) { return s_last_wheel; }
float platform_pen_pressure(void) { return s_pen_pressure; }

bool platform_touch_take(float* pan_x, float* pan_y, float* zoom, float* cx, float* cy) {
    if (!s_gesture.any) return false;
    *pan_x = s_gesture.pan_x;
    *pan_y = s_gesture.pan_y;
    *zoom = s_gesture.zoom;
    *cx = s_gesture.cx;
    *cy = s_gesture.cy;
    s_gesture.pan_x = s_gesture.pan_y = 0.0f;
    s_gesture.zoom = 1.0f;
    s_gesture.any = false;
    return true;
}

bool platform_touch_peek(int* fingers, float* cx, float* cy) {
    if (!s_gesture.any) return false;
    if (fingers) *fingers = s_gesture.fingers;
    if (cx) *cx = s_gesture.cx;
    if (cy) *cy = s_gesture.cy;
    return true;
}

bool platform_touch_screen(void) { return true; }

static bool g_want_text;  // ImGui is typing into a field: the keyboard's characters are text, not shortcuts
static bool g_keyboard_forced; // the touch bar's keyboard button: shown for shortcuts too
static bool g_hover_mode; // platform_touch_hover_mode

static void keyboard_show(void) {
    static int shown = -1;
    int want = g_want_text || g_keyboard_forced;
    if (want == shown) return;
    shown = want;
    re_android_call_void("showKeyboard", "(Z)V", want ? JNI_TRUE : JNI_FALSE);
}

void platform_text_input(bool want) {
    g_want_text = want;
    keyboard_show();
}

void re_android_keyboard_toggle(void) {
    g_keyboard_forced = !g_keyboard_forced;
    keyboard_show();
}
bool re_android_keyboard_forced(void) { return g_keyboard_forced; }

void platform_touch_hover_mode(bool on) { g_hover_mode = on; }

static int g_sticky_mods;
void platform_set_sticky_mods(int glfw_mods) { g_sticky_mods = glfw_mods; }

static float g_key_tx = -1.0f, g_key_ty = -1.0f;
void platform_set_key_target(float x, float y) {
    g_key_tx = x;
    g_key_ty = y;
}
static void move_cursor(float x, float y);
static void cursor_enter(int entered);
void platform_point_at(float x, float y) {
    if (!g_window || x < 0.0f || y < 0.0f) return;
    cursor_enter(1);
    g_window->off_x = g_window->off_y = 0;
    move_cursor(x, y);
}

// ---------------------------------------------------------------------------------------------------------------------
// JNI helpers
struct android_app* re_android_app(void) { return g_app; }
JNIEnv* re_android_env(void) { return g_env; }
jobject re_android_activity(void) { return g_activity; }
const char* re_android_files_dir(void) { return g_files_dir; }
const char* re_android_documents_dir(void) { return g_docs_dir; }
const char* re_android_cache_dir(void) { return g_cache_dir; }
float re_android_density(void) { return g_density; }
float re_android_screen_inches(void) { return g_inches; }

void re_android_set_surface_callbacks(void (*lost)(void* user), void (*gained)(void* user), void* user) {
    g_surface_lost = lost;
    g_surface_gained = gained;
    g_surface_user = user;
}

static jmethodID method(const char* name, const char* sig) {
    if (!g_env || !g_activity) return NULL;
    jclass cls = (*g_env)->GetObjectClass(g_env, g_activity);
    jmethodID m = (*g_env)->GetMethodID(g_env, cls, name, sig);
    (*g_env)->DeleteLocalRef(g_env, cls);
    if (!m) {
        (*g_env)->ExceptionClear(g_env);
        LOGW("método Java ausente: %s%s", name, sig);
    }
    return m;
}

static void clear_exception(void) {
    if (g_env && (*g_env)->ExceptionCheck(g_env)) {
        (*g_env)->ExceptionDescribe(g_env);
        (*g_env)->ExceptionClear(g_env);
    }
}

void re_android_call_void(const char* name, const char* sig, ...) {
    jmethodID m = method(name, sig);
    if (!m) return;
    va_list ap;
    va_start(ap, sig);
    (*g_env)->CallVoidMethodV(g_env, g_activity, m, ap);
    va_end(ap);
    clear_exception();
}

bool re_android_call_string(const char* name, const char* sig, char* out, size_t cap, ...) {
    if (cap) out[0] = 0;
    jmethodID m = method(name, sig);
    if (!m) return false;
    va_list ap;
    va_start(ap, cap);
    jstring s = (jstring)(*g_env)->CallObjectMethodV(g_env, g_activity, m, ap);
    va_end(ap);
    clear_exception();
    if (!s) return false;
    const char* u = (*g_env)->GetStringUTFChars(g_env, s, NULL);
    if (u) {
        snprintf(out, cap, "%s", u);
        (*g_env)->ReleaseStringUTFChars(g_env, s, u);
    }
    (*g_env)->DeleteLocalRef(g_env, s);
    return true;
}

// Java -> native (GameActivity's input connection, on the UI thread)
static void JNICALL native_text(JNIEnv* env, jobject self, jstring text) {
    (void)self;
    const jchar* s = (*env)->GetStringChars(env, text, NULL);
    jsize n = (*env)->GetStringLength(env, text);
    pthread_mutex_lock(&g_text_lock);
    for (jsize i = 0; i < n && g_text_n < (int)(sizeof g_text / sizeof g_text[0]); i++) {
        unsigned c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) { // surrogate pair
            unsigned lo = s[++i];
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        }
        if (c == '\n') {
            if (g_text_keys_n < 64) g_text_keys[g_text_keys_n++] = AKEYCODE_ENTER;
        } else {
            g_text[g_text_n++] = c;
        }
    }
    pthread_mutex_unlock(&g_text_lock);
    (*env)->ReleaseStringChars(env, text, s);
}

static void JNICALL native_key(JNIEnv* env, jobject self, jint keycode) {
    (void)env;
    (void)self;
    pthread_mutex_lock(&g_text_lock);
    if (g_text_keys_n < 64) g_text_keys[g_text_keys_n++] = keycode;
    pthread_mutex_unlock(&g_text_lock);
}

// Defined in platform_android.c (the file pickers' answers)
void JNICALL re_android_native_files_picked(JNIEnv* env, jobject self, jint request, jobjectArray paths);

// --- HTTP (net_http.c on Android): GameActivity.httpRequest on the calling thread, attached to the VM ------------
typedef struct HttpCtx {
    bool (*progress)(void* user, uint64_t done, uint64_t total);
    void* user;
} HttpCtx;

static jboolean JNICALL native_http_progress(JNIEnv* env, jclass cls, jlong ctx, jlong done, jlong total) {
    (void)env;
    (void)cls;
    const HttpCtx* h = (const HttpCtx*)(intptr_t)ctx;
    if (!h || !h->progress) return JNI_TRUE;
    return h->progress(h->user, (uint64_t)(done > 0 ? done : 0), (uint64_t)(total > 0 ? total : 0)) ? JNI_TRUE : JNI_FALSE;
}

int re_android_http(const char* method, const char* url, const char* headers, const void* body, size_t body_len,
                    const char* out_path, int timeout_ms, bool (*progress)(void*, uint64_t, uint64_t), void* user,
                    uint8_t** body_out, size_t* len_out, char* err, size_t err_cap) {
    if (body_out) *body_out = NULL;
    if (len_out) *len_out = 0;
    if (err_cap) err[0] = 0;
    if (!g_vm || !g_activity) {
        snprintf(err, err_cap, "sem a máquina Java");
        return 0;
    }
    JNIEnv* env = NULL;
    bool attached = false;
    if ((*g_vm)->GetEnv(g_vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) {
            snprintf(err, err_cap, "não foi possível ligar a thread ao Java");
            return 0;
        }
        attached = true;
    }
    int status = 0;
    (*env)->PushLocalFrame(env, 16);
    jclass cls = (*env)->GetObjectClass(env, g_activity);
    jmethodID m = (*env)->GetMethodID(env, cls, "httpRequest",
                                      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[BLjava/lang/String;IJ[[B[Ljava/lang/String;)I");
    if (!m) {
        (*env)->ExceptionClear(env);
        snprintf(err, err_cap, "httpRequest ausente no GameActivity");
    } else {
        jstring jm = (*env)->NewStringUTF(env, method ? method : "GET"), ju = (*env)->NewStringUTF(env, url);
        jstring jh = headers ? (*env)->NewStringUTF(env, headers) : NULL;
        jstring jo = out_path ? (*env)->NewStringUTF(env, out_path) : NULL;
        jbyteArray jb = NULL;
        if (body && body_len) {
            jb = (*env)->NewByteArray(env, (jsize)body_len);
            if (jb) (*env)->SetByteArrayRegion(env, jb, 0, (jsize)body_len, (const jbyte*)body);
        }
        jclass bcls = (*env)->FindClass(env, "[B"), scls = (*env)->FindClass(env, "java/lang/String");
        jobjectArray bout = (*env)->NewObjectArray(env, 1, bcls, NULL), eout = (*env)->NewObjectArray(env, 1, scls, NULL);
        HttpCtx ctx = { progress, user };
        status = (*env)->CallIntMethod(env, g_activity, m, jm, ju, jh, jb, jo, (jint)(timeout_ms > 0 ? timeout_ms : 30000),
                                       (jlong)(intptr_t)(progress ? &ctx : NULL), bout, eout);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            status = 0;
            snprintf(err, err_cap, "exceção no Java");
        }
        jstring je = (jstring)(*env)->GetObjectArrayElement(env, eout, 0);
        if (je) {
            const char* u = (*env)->GetStringUTFChars(env, je, NULL);
            if (u) snprintf(err, err_cap, "%s", u), (*env)->ReleaseStringUTFChars(env, je, u);
        }
        jbyteArray jr = (jbyteArray)(*env)->GetObjectArrayElement(env, bout, 0);
        if (jr && body_out) {
            jsize n = (*env)->GetArrayLength(env, jr);
            uint8_t* b = malloc((size_t)n + 1);
            if (b) {
                (*env)->GetByteArrayRegion(env, jr, 0, n, (jbyte*)b);
                b[n] = 0;
                *body_out = b;
                if (len_out) *len_out = (size_t)n;
            }
        }
    }
    (*env)->PopLocalFrame(env, NULL);
    if (attached) (*g_vm)->DetachCurrentThread(g_vm);
    return status;
}

// The JNI environment of any thread (attached here when it was not: release with re_android_thread_env_end)
JNIEnv* re_android_thread_env(bool* attached) {
    *attached = false;
    if (!g_vm) return NULL;
    JNIEnv* env = NULL;
    if ((*g_vm)->GetEnv(g_vm, (void**)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return NULL;
    *attached = true;
    return env;
}
void re_android_thread_env_end(bool attached) {
    if (attached && g_vm) (*g_vm)->DetachCurrentThread(g_vm);
}

static void register_natives(void) {
    jclass cls = (*g_env)->GetObjectClass(g_env, g_activity);
    JNINativeMethod m[] = {
        { "nativeText", "(Ljava/lang/String;)V", (void*)native_text },
        { "nativeKey", "(I)V", (void*)native_key },
        { "nativeHttpProgress", "(JJJ)Z", (void*)native_http_progress },
    };
    if ((*g_env)->RegisterNatives(g_env, cls, m, (jint)(sizeof m / sizeof m[0])) != 0) {
        clear_exception();
        LOGW("RegisterNatives falhou (GameActivity)");
    }
    (*g_env)->DeleteLocalRef(g_env, cls);
}

// ---------------------------------------------------------------------------------------------------------------------
// Keys
static int glfw_key(int32_t k) {
    if (k >= AKEYCODE_A && k <= AKEYCODE_Z) return GLFW_KEY_A + (k - AKEYCODE_A);
    if (k >= AKEYCODE_0 && k <= AKEYCODE_9) return GLFW_KEY_0 + (k - AKEYCODE_0);
    if (k >= AKEYCODE_F1 && k <= AKEYCODE_F12) return GLFW_KEY_F1 + (k - AKEYCODE_F1);
    if (k >= AKEYCODE_NUMPAD_0 && k <= AKEYCODE_NUMPAD_9) return GLFW_KEY_KP_0 + (k - AKEYCODE_NUMPAD_0);
    switch (k) {
    case AKEYCODE_SPACE: return GLFW_KEY_SPACE;
    case AKEYCODE_APOSTROPHE: return GLFW_KEY_APOSTROPHE;
    case AKEYCODE_COMMA: return GLFW_KEY_COMMA;
    case AKEYCODE_MINUS: return GLFW_KEY_MINUS;
    case AKEYCODE_PERIOD: return GLFW_KEY_PERIOD;
    case AKEYCODE_SLASH: return GLFW_KEY_SLASH;
    case AKEYCODE_SEMICOLON: return GLFW_KEY_SEMICOLON;
    case AKEYCODE_EQUALS: return GLFW_KEY_EQUAL;
    case AKEYCODE_LEFT_BRACKET: return GLFW_KEY_LEFT_BRACKET;
    case AKEYCODE_BACKSLASH: return GLFW_KEY_BACKSLASH;
    case AKEYCODE_RIGHT_BRACKET: return GLFW_KEY_RIGHT_BRACKET;
    case AKEYCODE_GRAVE: return GLFW_KEY_GRAVE_ACCENT;
    case AKEYCODE_ESCAPE: return GLFW_KEY_ESCAPE;
    case AKEYCODE_BACK: return GLFW_KEY_ESCAPE; // the back gesture cancels, as Esc does (it never closes the app)
    case AKEYCODE_ENTER: return GLFW_KEY_ENTER;
    case AKEYCODE_TAB: return GLFW_KEY_TAB;
    case AKEYCODE_DEL: return GLFW_KEY_BACKSPACE;
    case AKEYCODE_FORWARD_DEL: return GLFW_KEY_DELETE;
    case AKEYCODE_INSERT: return GLFW_KEY_INSERT;
    case AKEYCODE_DPAD_RIGHT: return GLFW_KEY_RIGHT;
    case AKEYCODE_DPAD_LEFT: return GLFW_KEY_LEFT;
    case AKEYCODE_DPAD_DOWN: return GLFW_KEY_DOWN;
    case AKEYCODE_DPAD_UP: return GLFW_KEY_UP;
    case AKEYCODE_PAGE_UP: return GLFW_KEY_PAGE_UP;
    case AKEYCODE_PAGE_DOWN: return GLFW_KEY_PAGE_DOWN;
    case AKEYCODE_MOVE_HOME: return GLFW_KEY_HOME;
    case AKEYCODE_MOVE_END: return GLFW_KEY_END;
    case AKEYCODE_CAPS_LOCK: return GLFW_KEY_CAPS_LOCK;
    case AKEYCODE_SCROLL_LOCK: return GLFW_KEY_SCROLL_LOCK;
    case AKEYCODE_NUM_LOCK: return GLFW_KEY_NUM_LOCK;
    case AKEYCODE_SYSRQ: return GLFW_KEY_PRINT_SCREEN;
    case AKEYCODE_BREAK: return GLFW_KEY_PAUSE;
    case AKEYCODE_NUMPAD_DOT: return GLFW_KEY_KP_DECIMAL;
    case AKEYCODE_NUMPAD_DIVIDE: return GLFW_KEY_KP_DIVIDE;
    case AKEYCODE_NUMPAD_MULTIPLY: return GLFW_KEY_KP_MULTIPLY;
    case AKEYCODE_NUMPAD_SUBTRACT: return GLFW_KEY_KP_SUBTRACT;
    case AKEYCODE_NUMPAD_ADD: return GLFW_KEY_KP_ADD;
    case AKEYCODE_NUMPAD_ENTER: return GLFW_KEY_KP_ENTER;
    case AKEYCODE_NUMPAD_EQUALS: return GLFW_KEY_KP_EQUAL;
    case AKEYCODE_SHIFT_LEFT: return GLFW_KEY_LEFT_SHIFT;
    case AKEYCODE_CTRL_LEFT: return GLFW_KEY_LEFT_CONTROL;
    case AKEYCODE_ALT_LEFT: return GLFW_KEY_LEFT_ALT;
    case AKEYCODE_META_LEFT: return GLFW_KEY_LEFT_SUPER;
    case AKEYCODE_SHIFT_RIGHT: return GLFW_KEY_RIGHT_SHIFT;
    case AKEYCODE_CTRL_RIGHT: return GLFW_KEY_RIGHT_CONTROL;
    case AKEYCODE_ALT_RIGHT: return GLFW_KEY_RIGHT_ALT;
    case AKEYCODE_META_RIGHT: return GLFW_KEY_RIGHT_SUPER;
    case AKEYCODE_MENU: return GLFW_KEY_MENU;
    default: return GLFW_KEY_UNKNOWN;
    }
}

static int glfw_mods(int32_t meta) {
    int m = 0;
    if (meta & AMETA_SHIFT_ON) m |= GLFW_MOD_SHIFT;
    if (meta & AMETA_CTRL_ON) m |= GLFW_MOD_CONTROL;
    if (meta & AMETA_ALT_ON) m |= GLFW_MOD_ALT;
    if (meta & AMETA_META_ON) m |= GLFW_MOD_SUPER;
    if (meta & AMETA_CAPS_LOCK_ON) m |= GLFW_MOD_CAPS_LOCK;
    if (meta & AMETA_NUM_LOCK_ON) m |= GLFW_MOD_NUM_LOCK;
    return m;
}

// The modifiers the app reads with glfwGetKey (Shift, Ctrl, Alt held on the on-screen touch bar count too).
static int current_mods(void) {
    GLFWwindow* w = g_window;
    int m = g_sticky_mods;
    if (!w) return m;
    if (w->keys[GLFW_KEY_LEFT_SHIFT] || w->keys[GLFW_KEY_RIGHT_SHIFT]) m |= GLFW_MOD_SHIFT;
    if (w->keys[GLFW_KEY_LEFT_CONTROL] || w->keys[GLFW_KEY_RIGHT_CONTROL]) m |= GLFW_MOD_CONTROL;
    if (w->keys[GLFW_KEY_LEFT_ALT] || w->keys[GLFW_KEY_RIGHT_ALT]) m |= GLFW_MOD_ALT;
    return m;
}

static void send_key(int key, int scancode, int action, int mods) {
    GLFWwindow* w = g_window;
    if (!w || key == GLFW_KEY_UNKNOWN) return;
    w->keys[key] = action != GLFW_RELEASE;
    if (w->key_cb) w->key_cb(w, key, scancode, action, mods);
}

// A key tapped (press and release) on behalf of the touch bar or the input method.
void re_android_tap_key(int glfw_key_code, int mods) {
    send_key(glfw_key_code, 0, GLFW_PRESS, mods | g_sticky_mods);
    send_key(glfw_key_code, 0, GLFW_RELEASE, mods | g_sticky_mods);
}

static void send_char(unsigned c) {
    if (g_window && g_window->char_cb && c >= 32 && c != 127) g_window->char_cb(g_window, c);
}

// The character a hardware key types (KeyEvent.getUnicodeChar through JNI).
static unsigned key_unicode(const AInputEvent* e) {
    if (!g_env) return 0;
    static jclass cls;
    static jmethodID ctor, get;
    if (!cls) {
        jclass local = (*g_env)->FindClass(g_env, "android/view/KeyEvent");
        if (!local) { clear_exception(); return 0; }
        cls = (*g_env)->NewGlobalRef(g_env, local);
        (*g_env)->DeleteLocalRef(g_env, local);
        ctor = (*g_env)->GetMethodID(g_env, cls, "<init>", "(JJIIIIIIII)V");
        get = (*g_env)->GetMethodID(g_env, cls, "getUnicodeChar", "(I)I");
        if (!ctor || !get) { clear_exception(); return 0; }
    }
    jobject ke = (*g_env)->NewObject(g_env, cls, ctor, (jlong)AKeyEvent_getDownTime(e), (jlong)AKeyEvent_getEventTime(e),
                                     (jint)AKeyEvent_getAction(e), (jint)AKeyEvent_getKeyCode(e),
                                     (jint)AKeyEvent_getRepeatCount(e), (jint)AKeyEvent_getMetaState(e),
                                     (jint)AInputEvent_getDeviceId(e), (jint)AKeyEvent_getScanCode(e),
                                     (jint)AKeyEvent_getFlags(e), (jint)AInputEvent_getSource(e));
    if (!ke) { clear_exception(); return 0; }
    jint c = (*g_env)->CallIntMethod(g_env, ke, get, (jint)AKeyEvent_getMetaState(e));
    (*g_env)->DeleteLocalRef(g_env, ke);
    clear_exception();
    return c > 0 ? (unsigned)c : 0;
}

static int32_t on_key(const AInputEvent* e) {
    int32_t code = AKeyEvent_getKeyCode(e);
    // volume and media keys stay with the system
    if (code == AKEYCODE_VOLUME_UP || code == AKEYCODE_VOLUME_DOWN || code == AKEYCODE_VOLUME_MUTE ||
        code == AKEYCODE_POWER || code == AKEYCODE_HOME || code == AKEYCODE_APP_SWITCH)
        return 0;
    int action = AKeyEvent_getAction(e);
    int key = glfw_key(code);
    int mods = glfw_mods(AKeyEvent_getMetaState(e)) | g_sticky_mods;
    if (action == AKEY_EVENT_ACTION_DOWN) {
        send_key(key, AKeyEvent_getScanCode(e), AKeyEvent_getRepeatCount(e) > 0 ? GLFW_REPEAT : GLFW_PRESS, mods);
        if (!(mods & (GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SUPER))) send_char(key_unicode(e));
    } else if (action == AKEY_EVENT_ACTION_UP) {
        send_key(key, AKeyEvent_getScanCode(e), GLFW_RELEASE, mods);
    }
    return 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// Pointer
static void move_cursor(float x, float y) {
    GLFWwindow* w = g_window;
    if (!w) return;
    w->cx = x + w->off_x;
    w->cy = y + w->off_y;
    if (w->cursor_cb) w->cursor_cb(w, w->cx, w->cy);
}

// A finger lifted leaves the screen: nothing stays hovered (no tooltip stuck on the button just tapped); the next touch
// enters again. A pen hovering and a mouse stay.
static void cursor_enter(int entered) {
    static int inside = 1;
    if (!g_window || inside == entered) return;
    inside = entered;
    if (g_window->enter_cb) g_window->enter_cb(g_window, entered);
}

static void send_button(int button, int action) {
    GLFWwindow* w = g_window;
    if (!w) return;
    if ((w->buttons[button] != 0) == (action == GLFW_PRESS)) return;
    w->buttons[button] = action == GLFW_PRESS;
    if (w->button_cb) w->button_cb(w, button, action, current_mods());
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// The fingers' centroid and mean distance to it.
static void finger_stats(const AInputEvent* e, int skip, float* gx, float* gy, float* gd, int* n_out) {
    size_t n = AMotionEvent_getPointerCount(e);
    float sx = 0, sy = 0;
    int n_used = 0;
    for (size_t i = 0; i < n; i++) {
        if ((int)i == skip) continue;
        sx += GX(e, i);
        sy += GY(e, i);
        n_used++;
    }
    if (!n_used) { *gx = *gy = *gd = 0; *n_out = 0; return; }
    sx /= (float)n_used;
    sy /= (float)n_used;
    float d = 0;
    for (size_t i = 0; i < n; i++) {
        if ((int)i == skip) continue;
        float dx = GX(e, i) - sx, dy = GY(e, i) - sy;
        d += sqrtf(dx * dx + dy * dy);
    }
    *gx = sx;
    *gy = sy;
    *gd = d / (float)n_used;
    *n_out = n_used;
}

static float touch_slop(void) { return 8.0f * g_density; } // ~8 dp before a still finger becomes a drag

static void touch_finger(const AInputEvent* e) {
    int32_t a = AMotionEvent_getAction(e);
    int act = a & AMOTION_EVENT_ACTION_MASK;
    int idx = (a & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    float x = GX(e, 0), y = GY(e, 0);
    s_pen_pressure = -1.0f;
    if (g_hover_mode && (s_touch.state == TOUCH_IDLE || s_touch.state == TOUCH_HOVER) && g_window) {
        // G / R / S running: the finger moves the cursor from where it is (a touchpad); lifting it confirms
        if (act == AMOTION_EVENT_ACTION_DOWN) {
            cursor_enter(1);
            g_window->off_x = g_window->cx - x;
            g_window->off_y = g_window->cy - y;
            s_touch.state = TOUCH_HOVER;
            s_touch.count = 1;
            return;
        }
        if (s_touch.state == TOUCH_HOVER) {
            if (act == AMOTION_EVENT_ACTION_MOVE) {
                move_cursor(x, y);
                return;
            }
            if (act == AMOTION_EVENT_ACTION_UP) {
                move_cursor(x, y);
                send_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS);
                send_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE);
                s_touch.state = TOUCH_IDLE;
                s_touch.count = 0;
                return;
            }
            if (act == AMOTION_EVENT_ACTION_CANCEL) {
                s_touch.state = TOUCH_IDLE;
                return;
            }
            // a second finger: the gesture (orbit while moving), the modal goes on
            s_touch.state = TOUCH_IDLE;
        }
    }
    switch (act) {
    case AMOTION_EVENT_ACTION_DOWN:
        if (g_window) g_window->off_x = g_window->off_y = 0; // a new touch is where the finger is
        cursor_enter(1);
        s_touch.state = TOUCH_PENDING;
        s_touch.t0 = now_seconds();
        s_touch.x0 = s_touch.x = x;
        s_touch.y0 = s_touch.y = y;
        s_touch.count = 1;
        move_cursor(x, y); // the hover moves first: ImGui sees the press where the finger is
        break;
    case AMOTION_EVENT_ACTION_POINTER_DOWN: {
        if (s_touch.state == TOUCH_DRAG) send_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE); // a drag turned gesture
        s_touch.state = TOUCH_GESTURE;
        int n;
        finger_stats(e, -1, &s_touch.gx, &s_touch.gy, &s_touch.gd, &n);
        s_touch.count = n;
        move_cursor(s_touch.gx, s_touch.gy);
        break;
    }
    case AMOTION_EVENT_ACTION_MOVE:
        if (s_touch.state == TOUCH_PENDING || s_touch.state == TOUCH_DRAG) {
            s_touch.x = x;
            s_touch.y = y;
            if (s_touch.state == TOUCH_PENDING) {
                float dx = x - s_touch.x0, dy = y - s_touch.y0;
                if (dx * dx + dy * dy > touch_slop() * touch_slop()) {
                    // the drag starts where the finger went down (a box select from its corner)
                    move_cursor(s_touch.x0, s_touch.y0);
                    send_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS);
                    s_touch.state = TOUCH_DRAG;
                }
            }
            if (s_touch.state == TOUCH_DRAG) move_cursor(x, y);
        } else if (s_touch.state == TOUCH_GESTURE) {
            float gx, gy, gd;
            int n;
            finger_stats(e, -1, &gx, &gy, &gd, &n);
            if (n >= 2 && n == s_touch.count) {
                s_gesture.pan_x += gx - s_touch.gx;
                s_gesture.pan_y += gy - s_touch.gy;
                if (s_touch.gd > 1.0f && gd > 1.0f) s_gesture.zoom *= gd / s_touch.gd;
                s_gesture.cx = gx;
                s_gesture.cy = gy;
                if (!s_gesture.any || n > s_gesture.fingers) s_gesture.fingers = n;
                s_gesture.any = true;
                move_cursor(gx, gy);
            }
            s_touch.gx = gx;
            s_touch.gy = gy;
            s_touch.gd = gd;
            s_touch.count = n;
        }
        break;
    case AMOTION_EVENT_ACTION_POINTER_UP: {
        int n;
        finger_stats(e, idx, &s_touch.gx, &s_touch.gy, &s_touch.gd, &n); // no jump when a finger leaves
        s_touch.count = n;
        break;
    }
    case AMOTION_EVENT_ACTION_UP:
        if (s_touch.state == TOUCH_PENDING) { // a tap: a click where the finger was
            move_cursor(s_touch.x0, s_touch.y0);
            send_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS);
            send_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE);
        } else if (s_touch.state == TOUCH_DRAG) {
            move_cursor(x, y);
            send_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE);
        }
        s_touch.state = TOUCH_IDLE;
        s_touch.count = 0;
        cursor_enter(0); // the finger left the screen: nothing hovered
        break;
    case AMOTION_EVENT_ACTION_CANCEL:
        if (s_touch.state == TOUCH_DRAG) send_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE);
        s_touch.state = TOUCH_IDLE;
        s_touch.count = 0;
        break;
    }
}

// Press and hold without moving: a right click (context menus), once per touch.
static void touch_long_press_tick(void) {
    if (s_touch.state != TOUCH_PENDING) return;
    if (now_seconds() - s_touch.t0 < 0.55) return;
    move_cursor(s_touch.x0, s_touch.y0);
    send_button(GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS);
    send_button(GLFW_MOUSE_BUTTON_RIGHT, GLFW_RELEASE);
    s_touch.state = TOUCH_DONE;
}

static int32_t g_pen_buttons, g_mouse_buttons;

static void pen_event(const AInputEvent* e) {
    int act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
    float x = GX(e, 0), y = GY(e, 0);
    int32_t bs = AMotionEvent_getButtonState(e);
    bool side = (bs & (AMOTION_EVENT_BUTTON_STYLUS_PRIMARY | AMOTION_EVENT_BUTTON_SECONDARY)) != 0;
    bool side2 = (bs & (AMOTION_EVENT_BUTTON_STYLUS_SECONDARY | AMOTION_EVENT_BUTTON_TERTIARY)) != 0;
    if (s_touch.state == TOUCH_IDLE && g_window) g_window->off_x = g_window->off_y = 0;
    switch (act) {
    case AMOTION_EVENT_ACTION_HOVER_ENTER:
    case AMOTION_EVENT_ACTION_HOVER_MOVE:
        s_pen_pressure = -1.0f;
        cursor_enter(1);
        move_cursor(x, y);
        break;
    case AMOTION_EVENT_ACTION_HOVER_EXIT: // the pen went away from the screen
        s_pen_pressure = -1.0f;
        cursor_enter(0);
        break;
    case AMOTION_EVENT_ACTION_DOWN:
        s_pen_pressure = AMotionEvent_getPressure(e, 0);
        cursor_enter(1);
        move_cursor(x, y);
        // the side button held: the middle button (orbit, Shift: pan, Ctrl: zoom), as with a tablet in Blender
        g_pen_buttons = side ? GLFW_MOUSE_BUTTON_MIDDLE : side2 ? GLFW_MOUSE_BUTTON_RIGHT : GLFW_MOUSE_BUTTON_LEFT;
        send_button(g_pen_buttons, GLFW_PRESS);
        break;
    case AMOTION_EVENT_ACTION_MOVE: {
        // the move's history (the pen reports faster than the frames): the strokes follow every sample
        size_t hn = AMotionEvent_getHistorySize(e);
        for (size_t h = 0; h < hn; h++) {
            s_pen_pressure = AMotionEvent_getHistoricalPressure(e, 0, h);
            move_cursor(AMotionEvent_getHistoricalX(e, 0, h), AMotionEvent_getHistoricalY(e, 0, h));
        }
        s_pen_pressure = AMotionEvent_getPressure(e, 0);
        move_cursor(x, y);
        break;
    }
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_CANCEL:
        move_cursor(x, y);
        send_button(g_pen_buttons, GLFW_RELEASE);
        s_pen_pressure = -1.0f;
        break;
    }
}

static void mouse_event(const AInputEvent* e) {
    int act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
    float x = GX(e, 0), y = GY(e, 0);
    s_pen_pressure = -1.0f;
    if (act == AMOTION_EVENT_ACTION_SCROLL) {
        float v = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_VSCROLL, 0);
        float h = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HSCROLL, 0);
        s_last_wheel = (fabsf(v) > 0 && fabsf(v) < 1.0f) || fabsf(h) > 0 ? PLATFORM_WHEEL_TOUCHPAD : PLATFORM_WHEEL_MOUSE;
        if (g_window && g_window->scroll_cb) g_window->scroll_cb(g_window, -h, v);
        return;
    }
    cursor_enter(1);
    move_cursor(x, y);
    int32_t bs = AMotionEvent_getButtonState(e);
    static const struct { int32_t bit; int button; } map[] = {
        { AMOTION_EVENT_BUTTON_PRIMARY, GLFW_MOUSE_BUTTON_LEFT },
        { AMOTION_EVENT_BUTTON_SECONDARY, GLFW_MOUSE_BUTTON_RIGHT },
        { AMOTION_EVENT_BUTTON_TERTIARY, GLFW_MOUSE_BUTTON_MIDDLE },
        { AMOTION_EVENT_BUTTON_BACK, GLFW_MOUSE_BUTTON_4 },
        { AMOTION_EVENT_BUTTON_FORWARD, GLFW_MOUSE_BUTTON_5 },
    };
    for (int i = 0; i < 5; i++) {
        bool was = (g_mouse_buttons & map[i].bit) != 0, is = (bs & map[i].bit) != 0;
        if (was != is) send_button(map[i].button, is ? GLFW_PRESS : GLFW_RELEASE);
    }
    g_mouse_buttons = bs;
}

// Every finger of the touch screen (the game's touch widgets: several at once)
static struct { int n; int id[10]; float xy[10][2]; } s_points;
static void points_update(const AInputEvent* e) {
    int32_t a = AMotionEvent_getAction(e);
    int act = a & AMOTION_EVENT_ACTION_MASK;
    int idx = (a & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    size_t n = AMotionEvent_getPointerCount(e);
    s_points.n = 0;
    if (act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_CANCEL) return;
    for (size_t i = 0; i < n && s_points.n < 10; i++) {
        if (act == AMOTION_EVENT_ACTION_POINTER_UP && (int)i == idx) continue;
        s_points.id[s_points.n] = (int)AMotionEvent_getPointerId(e, i);
        s_points.xy[s_points.n][0] = GX(e, i), s_points.xy[s_points.n][1] = GY(e, i);
        s_points.n++;
    }
}
int platform_touch_points(int* ids, float (*xy)[2], int cap) {
    int n = s_points.n < cap ? s_points.n : cap;
    for (int i = 0; i < n; i++) ids[i] = s_points.id[i], xy[i][0] = s_points.xy[i][0], xy[i][1] = s_points.xy[i][1];
    return n;
}
void re_vibrate_ms(int ms) { re_android_call_void("vibrate", "(I)V", (jint)ms); }

static int32_t on_input(struct android_app* app, AInputEvent* e) {
    (void)app;
    if (g_modal) return 1;
    int32_t type = AInputEvent_getType(e);
    if (type == AINPUT_EVENT_TYPE_KEY) return on_key(e);
    if (type != AINPUT_EVENT_TYPE_MOTION) return 0;
    int32_t tool = AMotionEvent_getToolType(e, 0);
    int32_t src = AInputEvent_getSource(e);
    if (tool == AMOTION_EVENT_TOOL_TYPE_STYLUS || tool == AMOTION_EVENT_TOOL_TYPE_ERASER) pen_event(e);
    else if (tool == AMOTION_EVENT_TOOL_TYPE_MOUSE || (src & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE) mouse_event(e);
    else points_update(e), touch_finger(e);
    return 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// Lifecycle
static void set_frame_rate(ANativeWindow* win, float fps) { // (Android 11's ANativeWindow_setFrameRate, loaded by name)
    static int32_t (*fn)(ANativeWindow*, float, int8_t);
    static int looked;
    if (!looked) {
        looked = 1;
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        if (lib) fn = (int32_t (*)(ANativeWindow*, float, int8_t))dlsym(lib, "ANativeWindow_setFrameRate");
    }
    if (fn && win && fps > 0.0f) fn(win, fps, 1); // (1: ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE, a game's pace)
}
static void window_apply(ANativeWindow* win) { // the scale and the frame rate on a (new) window
    if (!win) return;
    ANativeWindow_setBuffersGeometry(win, 0, 0, 0); // (back to the screen's size: measured)
    int w = ANativeWindow_getWidth(win), h = ANativeWindow_getHeight(win);
    if (w > 0 && h > 0 && g_content_scale < 0.999f) {
        int sw = (int)(w * g_content_scale + 0.5f) & ~1, sh = (int)(h * g_content_scale + 0.5f) & ~1;
        ANativeWindow_setBuffersGeometry(win, sw, sh, 0);
        g_in_scale = (float)sw / (float)w;
    } else g_in_scale = 1.0f;
    set_frame_rate(win, g_frame_rate);
}
void re_android_set_content_scale(float s) {
    g_content_scale = s > 0.3f && s <= 1.0f ? s : 1.0f;
    if (g_app && g_app->window) window_apply(g_app->window);
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(NULL, &fw, &fh);
    LOGI("janela a %.0f%%: buffers %dx%d", g_content_scale * 100.0f, fw, fh);
}
void re_android_set_frame_rate(float fps) {
    g_frame_rate = fps;
    if (g_app && g_app->window) set_frame_rate(g_app->window, fps);
}
int re_android_low_memory_count(void) { return g_low_memory; }

static void on_cmd(struct android_app* app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_LOW_MEMORY: g_low_memory++; break;
    case APP_CMD_WINDOW_RESIZED: window_apply(app->window); break; // (turned: the buffers' size again)
    case APP_CMD_INIT_WINDOW:
        window_apply(app->window);
        LOGI("janela criada %dx%d", app->window ? ANativeWindow_getWidth(app->window) : 0,
             app->window ? ANativeWindow_getHeight(app->window) : 0);
        if (g_surface_gained && app->window) g_surface_gained(g_surface_user);
        break;
    case APP_CMD_TERM_WINDOW:
        LOGI("janela destruída");
        if (g_surface_lost) g_surface_lost(g_surface_user);
        break;
    case APP_CMD_GAINED_FOCUS:
        if (g_window) {
            g_window->focused = 1;
            if (g_window->focus_cb) g_window->focus_cb(g_window, 1);
        }
        break;
    case APP_CMD_LOST_FOCUS:
        if (g_window) {
            g_window->focused = 0;
            // nothing stays held: the keys and buttons go up (the app does not see their release in the background)
            for (int b = 0; b <= GLFW_MOUSE_BUTTON_LAST; b++) send_button(b, GLFW_RELEASE);
            for (int k = 0; k <= GLFW_KEY_LAST; k++)
                if (g_window->keys[k]) send_key(k, 0, GLFW_RELEASE, 0);
            s_touch.state = TOUCH_IDLE;
            if (g_window->focus_cb) g_window->focus_cb(g_window, 0);
        }
        break;
    case APP_CMD_RESUME: g_resumed = 1; if (g_pause_cb) g_pause_cb(false); break;
    case APP_CMD_PAUSE: g_resumed = 0; if (g_pause_cb) g_pause_cb(true); break;
    case APP_CMD_SAVE_STATE:
        // the app may be killed in the background: the emergency save (autosave) is the app's (app_update.c)
        break;
    case APP_CMD_CONFIG_CHANGED:
        AConfiguration_fromAssetManager(app->config, app->activity->assetManager);
        break;
    case APP_CMD_DESTROY:
        if (g_window) {
            g_window->should_close = 1;
            if (g_window->close_cb) g_window->close_cb(g_window);
        }
        break;
    }
}

static void pump(int timeout_ms) {
    for (;;) {
        int events = 0;
        struct android_poll_source* source = NULL;
        int r = ALooper_pollOnce(timeout_ms, NULL, &events, (void**)&source);
        if (r == ALOOPER_POLL_ERROR || r == ALOOPER_POLL_TIMEOUT) break;
        if (source) source->process(g_app, source);
        if (g_app->destroyRequested) break;
        timeout_ms = 0; // after the first event, only what is already queued
    }
}

static void drain_text(void) {
    unsigned text[256];
    int keys[64], nt, nk;
    pthread_mutex_lock(&g_text_lock);
    nt = g_text_n;
    nk = g_text_keys_n;
    memcpy(text, g_text, (size_t)nt * sizeof text[0]);
    memcpy(keys, g_text_keys, (size_t)nk * sizeof keys[0]);
    g_text_n = g_text_keys_n = 0;
    pthread_mutex_unlock(&g_text_lock);
    for (int i = 0; i < nk; i++) re_android_tap_key(glfw_key(keys[i]), 0);
    for (int i = 0; i < nt; i++) {
        unsigned c = text[i];
        if (g_want_text || c > 127) {
            send_char(c);
            continue;
        }
        // no field typed in: the on-screen keyboard's letters are shortcuts (G, Shift+D, 1 for the front view...)
        int key = GLFW_KEY_UNKNOWN, mods = 0;
        if (c >= 'a' && c <= 'z') key = GLFW_KEY_A + (int)(c - 'a');
        else if (c >= 'A' && c <= 'Z') { key = GLFW_KEY_A + (int)(c - 'A'); mods = GLFW_MOD_SHIFT; }
        else if (c >= '0' && c <= '9') key = GLFW_KEY_0 + (int)(c - '0');
        else if (c == ' ') key = GLFW_KEY_SPACE;
        else if (c == '.') key = GLFW_KEY_PERIOD;
        else if (c == ',') key = GLFW_KEY_COMMA;
        else if (c == '-') key = GLFW_KEY_MINUS;
        else if (c == '/') key = GLFW_KEY_SLASH;
        else if (c == '`' || c == '~') key = GLFW_KEY_GRAVE_ACCENT;
        if (key == GLFW_KEY_UNKNOWN) continue;
        platform_point_at(g_key_tx, g_key_ty); // the editor last touched gets the shortcut
        if (mods & GLFW_MOD_SHIFT) send_key(GLFW_KEY_LEFT_SHIFT, 0, GLFW_PRESS, mods);
        re_android_tap_key(key, mods);
        if (mods & GLFW_MOD_SHIFT) send_key(GLFW_KEY_LEFT_SHIFT, 0, GLFW_RELEASE, 0);
    }
}

void re_android_wait(volatile int* done) {
    g_modal++;
    while (!*done && !g_app->destroyRequested) pump(50);
    g_modal--;
}

// ---------------------------------------------------------------------------------------------------------------------
// GLFW API
int glfwInit(void) {
    g_time0 = now_seconds();
    return GLFW_TRUE;
}
void glfwTerminate(void) {}
const char* glfwGetVersionString(void) { return "3.4.0 Android (RealEngine)"; }
int glfwGetError(const char** description) {
    if (description) *description = NULL;
    return GLFW_NO_ERROR;
}
GLFWerrorfun glfwSetErrorCallback(GLFWerrorfun cb) {
    GLFWerrorfun old = g_error_cb;
    g_error_cb = cb;
    return old;
}
int glfwGetPlatform(void) { return GLFW_PLATFORM_NULL; }
int glfwVulkanSupported(void) { return GLFW_TRUE; }
void glfwWindowHint(int hint, int value) { (void)hint; (void)value; }

const char** glfwGetRequiredInstanceExtensions(uint32_t* count) {
    static const char* exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
    *count = 2;
    return exts;
}

GLFWwindow* glfwCreateWindow(int width, int height, const char* title, GLFWmonitor* monitor, GLFWwindow* share) {
    (void)width; (void)height; (void)title; (void)monitor; (void)share;
    // the activity's window: wait for it (it arrives a moment after the activity starts)
    while (!g_app->window && !g_app->destroyRequested) pump(100);
    if (!g_app->window) return NULL;
    memset(&g_window_storage, 0, sizeof g_window_storage);
    g_window = &g_window_storage;
    g_window->focused = 1;
    g_window->cursor_mode = GLFW_CURSOR_NORMAL;
    return g_window;
}

void glfwDestroyWindow(GLFWwindow* w) {
    if (w == g_window) g_window = NULL;
}

VkResult glfwCreateWindowSurface(VkInstance instance, GLFWwindow* window, const VkAllocationCallbacks* alloc,
                                 VkSurfaceKHR* surface) {
    (void)window;
    if (!g_app->window) return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
    VkAndroidSurfaceCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
    ci.window = g_app->window;
    return vkCreateAndroidSurfaceKHR(instance, &ci, alloc, surface);
}

void glfwPollEvents(void) {
    pump(0);
    drain_text();
    touch_long_press_tick();
}

void glfwWaitEvents(void) {
    pump(g_app->window && g_resumed ? 16 : 250);
    drain_text();
    touch_long_press_tick();
}

int glfwWindowShouldClose(GLFWwindow* w) { return w->should_close; }
void glfwSetWindowShouldClose(GLFWwindow* w, int value) { w->should_close = value; }
void glfwSetWindowUserPointer(GLFWwindow* w, void* p) { w->user = p; }
void* glfwGetWindowUserPointer(GLFWwindow* w) { return w->user; }
void glfwSetWindowTitle(GLFWwindow* w, const char* title) { (void)w; (void)title; }
void glfwShowWindow(GLFWwindow* w) { (void)w; }
void glfwMaximizeWindow(GLFWwindow* w) { (void)w; }
void glfwFocusWindow(GLFWwindow* w) { (void)w; }
void glfwSetWindowSize(GLFWwindow* w, int width, int height) { (void)w; (void)width; (void)height; }
void glfwGetWindowPos(GLFWwindow* w, int* x, int* y) {
    (void)w;
    if (x) *x = 0;
    if (y) *y = 0;
}

void glfwGetFramebufferSize(GLFWwindow* w, int* width, int* height) {
    (void)w;
    int fw = 0, fh = 0;
    if (g_app->window && g_resumed) {
        fw = ANativeWindow_getWidth(g_app->window);
        fh = ANativeWindow_getHeight(g_app->window);
        if (g_content_scale < 0.999f) // (the buffers' size: getWidth keeps the screen's after setBuffersGeometry)
            fw = (int)(fw * g_content_scale + 0.5f) & ~1, fh = (int)(fh * g_content_scale + 0.5f) & ~1;
    }
    if (width) *width = fw;
    if (height) *height = fh;
}
void glfwGetWindowSize(GLFWwindow* w, int* width, int* height) { glfwGetFramebufferSize(w, width, height); }

void glfwGetWindowContentScale(GLFWwindow* w, float* xs, float* ys) {
    (void)w;
    if (xs) *xs = g_density;
    if (ys) *ys = g_density;
}
void glfwGetMonitorContentScale(GLFWmonitor* m, float* xs, float* ys) { glfwGetWindowContentScale(NULL, xs, ys); (void)m; }
GLFWmonitorfun glfwSetMonitorCallback(GLFWmonitorfun cb) { (void)cb; return NULL; }
// (the game's window and monitor calls: one monitor, the screen; the window is always it)
static int g_monitor_dummy;
static GLFWvidmode g_mode;
GLFWmonitor* glfwGetPrimaryMonitor(void) { return (GLFWmonitor*)&g_monitor_dummy; }
GLFWmonitor** glfwGetMonitors(int* count) {
    static GLFWmonitor* list[1];
    list[0] = glfwGetPrimaryMonitor();
    if (count) *count = 1;
    return list;
}
void glfwGetMonitorPos(GLFWmonitor* m, int* x, int* y) {
    (void)m;
    if (x) *x = 0;
    if (y) *y = 0;
}
void glfwGetMonitorWorkarea(GLFWmonitor* m, int* x, int* y, int* w, int* h) {
    (void)m;
    int fw, fh;
    glfwGetFramebufferSize(NULL, &fw, &fh);
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = fw;
    if (h) *h = fh;
}
const GLFWvidmode* glfwGetVideoMode(GLFWmonitor* m) {
    (void)m;
    glfwGetFramebufferSize(NULL, &g_mode.width, &g_mode.height);
    g_mode.redBits = g_mode.greenBits = g_mode.blueBits = 8, g_mode.refreshRate = 60;
    return &g_mode;
}
// (the options' Resolution list: the phone's screen, its one mode)
const GLFWvidmode* glfwGetVideoModes(GLFWmonitor* m, int* count) {
    const GLFWvidmode* v = glfwGetVideoMode(m);
    if (count) *count = 1;
    return v;
}
void glfwSetWindowMonitor(GLFWwindow* w, GLFWmonitor* m, int x, int y, int width, int height, int rate) {
    (void)w, (void)m, (void)x, (void)y, (void)width, (void)height, (void)rate;
}
void glfwSetWindowAttrib(GLFWwindow* w, int attrib, int value) { (void)w, (void)attrib, (void)value; }
void glfwSetWindowPos(GLFWwindow* w, int x, int y) { (void)w, (void)x, (void)y; }
GLFWwindowposfun glfwSetWindowPosCallback(GLFWwindow* w, GLFWwindowposfun cb) { (void)w, (void)cb; return NULL; }
GLFWwindowsizefun glfwSetWindowSizeCallback(GLFWwindow* w, GLFWwindowsizefun cb) { (void)w, (void)cb; return NULL; }
void glfwSetWindowOpacity(GLFWwindow* w, float o) { (void)w, (void)o; }
void glfwMakeContextCurrent(GLFWwindow* w) { (void)w; }
void glfwSwapInterval(int i) { (void)i; }
void glfwSwapBuffers(GLFWwindow* w) { (void)w; }
int glfwRawMouseMotionSupported(void) { return GLFW_FALSE; }
int glfwJoystickIsGamepad(int jid) {
    (void)jid;
    return GLFW_FALSE;
}

int glfwGetWindowAttrib(GLFWwindow* w, int attrib) {
    switch (attrib) {
    case GLFW_FOCUSED: return w->focused;
    case GLFW_ICONIFIED: return !g_app->window || !g_resumed;
    case GLFW_VISIBLE: return GLFW_TRUE;
    case GLFW_MAXIMIZED: return GLFW_TRUE;
    case GLFW_HOVERED: return GLFW_TRUE;
    default: return 0;
    }
}

GLFWwindowclosefun glfwSetWindowCloseCallback(GLFWwindow* w, GLFWwindowclosefun cb) { GLFWwindowclosefun o = w->close_cb; w->close_cb = cb; return o; }
GLFWwindowfocusfun glfwSetWindowFocusCallback(GLFWwindow* w, GLFWwindowfocusfun cb) { GLFWwindowfocusfun o = w->focus_cb; w->focus_cb = cb; return o; }
GLFWcursorposfun glfwSetCursorPosCallback(GLFWwindow* w, GLFWcursorposfun cb) { GLFWcursorposfun o = w->cursor_cb; w->cursor_cb = cb; return o; }
GLFWcursorenterfun glfwSetCursorEnterCallback(GLFWwindow* w, GLFWcursorenterfun cb) { GLFWcursorenterfun o = w->enter_cb; w->enter_cb = cb; return o; }
GLFWmousebuttonfun glfwSetMouseButtonCallback(GLFWwindow* w, GLFWmousebuttonfun cb) { GLFWmousebuttonfun o = w->button_cb; w->button_cb = cb; return o; }
GLFWscrollfun glfwSetScrollCallback(GLFWwindow* w, GLFWscrollfun cb) { GLFWscrollfun o = w->scroll_cb; w->scroll_cb = cb; return o; }
GLFWkeyfun glfwSetKeyCallback(GLFWwindow* w, GLFWkeyfun cb) { GLFWkeyfun o = w->key_cb; w->key_cb = cb; return o; }
GLFWcharfun glfwSetCharCallback(GLFWwindow* w, GLFWcharfun cb) { GLFWcharfun o = w->char_cb; w->char_cb = cb; return o; }
GLFWdropfun glfwSetDropCallback(GLFWwindow* w, GLFWdropfun cb) { GLFWdropfun o = w->drop_cb; w->drop_cb = cb; return o; }

int glfwGetKey(GLFWwindow* w, int key) {
    if (key < 0 || key > GLFW_KEY_LAST) return GLFW_RELEASE;
    // the touch bar's held modifiers read as keys held
    if ((key == GLFW_KEY_LEFT_SHIFT && (g_sticky_mods & GLFW_MOD_SHIFT)) ||
        (key == GLFW_KEY_LEFT_CONTROL && (g_sticky_mods & GLFW_MOD_CONTROL)) ||
        (key == GLFW_KEY_LEFT_ALT && (g_sticky_mods & GLFW_MOD_ALT)))
        return GLFW_PRESS;
    return w->keys[key] ? GLFW_PRESS : GLFW_RELEASE;
}
int glfwGetMouseButton(GLFWwindow* w, int b) { return b >= 0 && b <= GLFW_MOUSE_BUTTON_LAST && w->buttons[b]; }
const char* glfwGetKeyName(int key, int scancode) { (void)key; (void)scancode; return NULL; }

void glfwGetCursorPos(GLFWwindow* w, double* x, double* y) {
    if (x) *x = w->cx;
    if (y) *y = w->cy;
}
void glfwSetCursorPos(GLFWwindow* w, double x, double y) {
    // no pointer to move: the offset makes the next motion continue from (x, y) (the number fields' endless drag)
    w->off_x += x - w->cx;
    w->off_y += y - w->cy;
    w->cx = x;
    w->cy = y;
}
void glfwSetInputMode(GLFWwindow* w, int mode, int value) {
    if (mode == GLFW_CURSOR) w->cursor_mode = value;
}
int glfwGetInputMode(GLFWwindow* w, int mode) { return mode == GLFW_CURSOR ? w->cursor_mode : 0; }

GLFWcursor* glfwCreateStandardCursor(int shape) {
    GLFWcursor* c = calloc(1, sizeof *c);
    if (c) c->shape = shape;
    return c;
}
GLFWcursor* glfwCreateCursor(const GLFWimage* image, int xhot, int yhot) {
    (void)image; (void)xhot; (void)yhot;
    return glfwCreateStandardCursor(GLFW_ARROW_CURSOR);
}
void glfwDestroyCursor(GLFWcursor* c) { free(c); }
void glfwSetCursor(GLFWwindow* w, GLFWcursor* c) { (void)w; (void)c; }

void glfwSetClipboardString(GLFWwindow* w, const char* s) {
    (void)w;
    free(g_clipboard);
    g_clipboard = strdup(s ? s : "");
    if (!g_env) return;
    jstring js = (*g_env)->NewStringUTF(g_env, g_clipboard);
    re_android_call_void("setClipboard", "(Ljava/lang/String;)V", js);
    (*g_env)->DeleteLocalRef(g_env, js);
}
const char* glfwGetClipboardString(GLFWwindow* w) {
    (void)w;
    static char buf[64 * 1024];
    if (re_android_call_string("getClipboard", "()Ljava/lang/String;", buf, sizeof buf)) return buf;
    return g_clipboard ? g_clipboard : "";
}

double glfwGetTime(void) { return now_seconds() - g_time0; }

int glfwGetGamepadState(int jid, GLFWgamepadstate* state) { (void)jid; (void)state; return GLFW_FALSE; }
const float* glfwGetJoystickAxes(int jid, int* count) { (void)jid; *count = 0; return NULL; }
const unsigned char* glfwGetJoystickButtons(int jid, int* count) { (void)jid; *count = 0; return NULL; }

// ---------------------------------------------------------------------------------------------------------------------
// Entry: NativeActivity calls android_main on its own thread; the app's main() runs there.
int main(int argc, char** argv);

static void read_dirs(void) {
    re_android_call_string("filesDir", "()Ljava/lang/String;", g_files_dir, sizeof g_files_dir);
    re_android_call_string("documentsDir", "()Ljava/lang/String;", g_docs_dir, sizeof g_docs_dir);
    re_android_call_string("cacheDirPath", "()Ljava/lang/String;", g_cache_dir, sizeof g_cache_dir);
    if (!g_files_dir[0] && g_app->activity->internalDataPath)
        snprintf(g_files_dir, sizeof g_files_dir, "%s", g_app->activity->internalDataPath);
    if (!g_docs_dir[0]) snprintf(g_docs_dir, sizeof g_docs_dir, "%s", g_files_dir);
    if (!g_cache_dir[0]) snprintf(g_cache_dir, sizeof g_cache_dir, "%s/cache", g_files_dir);
    mkdir(g_docs_dir, 0770);
    mkdir(g_cache_dir, 0770);
}

static void read_screen(void) {
    int32_t dpi = AConfiguration_getDensity(g_app->config);
    if (dpi <= 0 || dpi == ACONFIGURATION_DENSITY_ANY || dpi == ACONFIGURATION_DENSITY_NONE) dpi = 160;
    g_density = (float)dpi / 160.0f;
    char inches[32];
    if (re_android_call_string("screenInches", "()Ljava/lang/String;", inches, sizeof inches) && inches[0])
        g_inches = (float)atof(inches);
    LOGI("densidade %.2f, tela %.1f\"", g_density, g_inches);
}

// The data folder (release notes, branding, color management, templates) from the APK's assets into the files folder,
// once per version: the app reads it as on the PC, next to the "executable" (update_exe_dir() = files folder).
void re_android_extract_assets(void);

// The app's printf/fprintf (the startup marks, errors) into logcat ("adb logcat -s Tlender"), one line each.
static int g_log_pipe[2];
static void* log_thread(void* arg) {
    (void)arg;
    char buf[2048];
    size_t n = 0;
    for (;;) {
        ssize_t r = read(g_log_pipe[0], buf + n, sizeof buf - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        buf[n] = 0;
        char* start = buf;
        char* nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = 0;
            __android_log_write(ANDROID_LOG_INFO, "RealEngine", start);
            start = nl + 1;
        }
        n = strlen(start);
        memmove(buf, start, n + 1);
        if (n >= sizeof buf - 1) { // a very long line: out as it is
            __android_log_write(ANDROID_LOG_INFO, "RealEngine", buf);
            n = 0;
        }
    }
    return NULL;
}

static void redirect_stdio(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (pipe(g_log_pipe) != 0) return;
    dup2(g_log_pipe[1], 1);
    dup2(g_log_pipe[1], 2);
    pthread_t t;
    if (pthread_create(&t, NULL, log_thread, NULL) == 0) pthread_detach(t);
}

void android_main(struct android_app* app) {
    redirect_stdio();
    g_app = app;
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    g_vm = app->activity->vm;
    (*g_vm)->AttachCurrentThread(g_vm, &g_env, NULL);
    g_activity = app->activity->clazz;
    register_natives();
    read_dirs();
    read_screen();
    if (chdir(g_files_dir) != 0) LOGW("chdir(%s) falhou", g_files_dir);
    re_android_extract_assets();
    re_android_read_test_extras();
    LOGI("RealEngine: o jogo em %s/game", g_files_dir);

    char arg0[] = "realengine_game";
    char* argv[] = { arg0, NULL };
    int code = main(1, argv);
    LOGI("main() terminou (%d)", code);
    // The app's state is global and not made to start twice in one process: the process ends with the activity.
    ANativeActivity_finish(app->activity);
    while (!app->destroyRequested) pump(100);
    (*g_vm)->DetachCurrentThread(g_vm);
    _exit(code);
}
