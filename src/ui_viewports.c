// The windows dragged out of the editor as Unreal's floating windows (ui_viewports.h; 2026-11-25, item 9).
// Before, GLFW made each one a window of its own with no owner: a click on the editor put the editor above it (the
// Material Editor on the other side of the screen vanished behind it), each had its button on the taskbar, and the
// backend's swapchain asked for mailbox (its own FIXME: half the frame rate with a second window).
#include "ui_viewports.h"

#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <cimgui_impl.h>

static struct {
    GLFWwindow* main;
    HWND main_hwnd;
    void (*platform_create)(ImGuiViewport* vp);
    void (*renderer_create)(ImGuiViewport* vp);
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    uint32_t queue_family, min_images;
    VkPresentModeKHR present;
} U;

static void platform_create(ImGuiViewport* vp) {
    U.platform_create(vp);
    HWND h = (HWND)vp->PlatformHandleRaw;
    if (!h && vp->PlatformHandle) h = glfwGetWin32Window((GLFWwindow*)vp->PlatformHandle);
    // the owner: Windows keeps an owned window above its owner, hides and shows it with it, and gives it no taskbar
    // button (the editor's own button stands for all, as Unreal's). Set while the window is still hidden (GLFW_VISIBLE
    // false: the backend shows it later)
    if (h && U.main_hwnd && h != U.main_hwnd) SetWindowLongPtrW(h, GWLP_HWNDPARENT, (LONG_PTR)U.main_hwnd);
}

static void renderer_create(ImGuiViewport* vp) {
    U.renderer_create(vp);
    ImGui_ImplVulkanH_Window* wd = vp->RendererUserData ? ImGui_ImplVulkanH_GetWindowDataFromViewport(vp) : NULL;
    if (!wd || !wd->Swapchain || wd->PresentMode == U.present) return;
    VkPresentModeKHR want = ImGui_ImplVulkanH_SelectPresentMode(U.gpu, wd->Surface, &U.present, 1);
    if (want != U.present || want == wd->PresentMode) return; // (not offered by the surface: the backend's choice stays)
    wd->PresentMode = want; // (kept: the resizes build the swapchain again with it)
    ImGui_ImplVulkanH_CreateOrResizeWindow(U.instance, U.gpu, U.device, wd, U.queue_family, NULL, (int)vp->Size.x, (int)vp->Size.y,
                                           U.min_images, 0);
}

void ui_viewports_install(GLFWwindow* main_window, void* vk_instance, void* vk_gpu, void* vk_device, uint32_t queue_family,
                          uint32_t min_image_count, int present) {
    ImGuiIO* io = igGetIO_Nil();
    // no button of their own on the taskbar, the main window their parent viewport. (Auto-merge stays: a window over
    // the editor is drawn inside its window, above it anyway; NoAutoMerge made every floating window a window of the
    // system even over the editor, and the dock test's drags onto the panels' targets docked nothing)
    io->ConfigViewportsNoTaskBarIcon = true;
    io->ConfigViewportsNoDefaultParent = false;
    io->ConfigViewportsNoAutoMerge = false;
    // the other monitor's DPI: its windows and their fonts at its scale (theme.c's FontScaleDpi, overwritten per monitor)
    io->ConfigDpiScaleFonts = true;
    io->ConfigDpiScaleViewports = true;
    const char* e = getenv("REALENGINE_VP_PRESENT");
    if (e && !strcmp(e, "mailbox")) present = UIVP_PRESENT_MAILBOX;
    else if (e && !strcmp(e, "immediate")) present = UIVP_PRESENT_IMMEDIATE;
    else if (e && !strcmp(e, "fifo")) present = UIVP_PRESENT_FIFO;
    U.present = present == UIVP_PRESENT_MAILBOX ? VK_PRESENT_MODE_MAILBOX_KHR
                : present == UIVP_PRESENT_IMMEDIATE ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    U.main = main_window;
    U.main_hwnd = main_window ? glfwGetWin32Window(main_window) : NULL;
    U.instance = (VkInstance)vk_instance, U.gpu = (VkPhysicalDevice)vk_gpu, U.device = (VkDevice)vk_device;
    U.queue_family = queue_family, U.min_images = min_image_count;
    ImGuiPlatformIO* pio = igGetPlatformIO_Nil();
    if (pio->Platform_CreateWindow && pio->Platform_CreateWindow != platform_create)
        U.platform_create = pio->Platform_CreateWindow, pio->Platform_CreateWindow = platform_create;
    if (pio->Renderer_CreateWindow && pio->Renderer_CreateWindow != renderer_create)
        U.renderer_create = pio->Renderer_CreateWindow, pio->Renderer_CreateWindow = renderer_create;
}

static ImGuiViewport* secondary(int i) {
    ImGuiPlatformIO* pio = igGetPlatformIO_Nil();
    int k = 0;
    for (int j = 0; pio && j < pio->Viewports.Size; j++) {
        ImGuiViewport* v = pio->Viewports.Data[j];
        if (v == igGetMainViewport() || !v->PlatformHandle) continue;
        if (k++ == i) return v;
    }
    return NULL;
}

int ui_viewports_count(void) {
    int n = 0;
    while (secondary(n)) n++;
    return n;
}

void* ui_viewports_hwnd(int i) {
    ImGuiViewport* v = secondary(i);
    return v ? (void*)glfwGetWin32Window((GLFWwindow*)v->PlatformHandle) : NULL;
}

void* ui_viewports_owner(int i) {
    HWND h = (HWND)ui_viewports_hwnd(i);
    return h ? (void*)GetWindow(h, GW_OWNER) : NULL;
}

void* ui_viewports_main_hwnd(void) { return U.main_hwnd; }
int ui_viewports_wanted_present(void) { return (int)U.present; }

int ui_viewports_present_mode(int i) {
    ImGuiViewport* v = secondary(i);
    ImGui_ImplVulkanH_Window* wd = v && v->RendererUserData ? ImGui_ImplVulkanH_GetWindowDataFromViewport(v) : NULL;
    return wd ? (int)wd->PresentMode : -1;
}
