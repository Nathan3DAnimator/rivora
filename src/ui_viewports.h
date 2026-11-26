// The windows dragged out of the editor (ImGui's viewports) as Unreal's floating windows (2026-11-25, item 9): real
// windows of the system owned by the editor's main window (above it, never behind it after a click on the editor, no
// button of their own on the taskbar, minimized and closed with it), their present mode chosen (mailbox: measured, FIFO
// held the whole editor at the monitor's rate, 437 -> 165 fps with 2 windows; mailbox 458 -> 391) and in SDR.
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct GLFWwindow;

enum { UIVP_PRESENT_FIFO = 0, UIVP_PRESENT_MAILBOX, UIVP_PRESENT_IMMEDIATE };

// After ImGui_ImplGlfw_InitForVulkan and ImGui_ImplVulkan_Init: the platform's and the renderer's CreateWindow wrapped
// (external/ untouched), io's viewport options set. present: UIVP_PRESENT_* for the secondary windows
// (REALENGINE_VP_PRESENT=fifo|mailbox|immediate overrides it: the measures)
void ui_viewports_install(struct GLFWwindow* main_window, void* vk_instance, void* vk_gpu, void* vk_device,
                          uint32_t queue_family, uint32_t min_image_count, int present);

// The secondary windows now (the test): their count, and each one's system handle (HWND) and owner (GW_OWNER)
int ui_viewports_count(void);
void* ui_viewports_hwnd(int i);
void* ui_viewports_owner(int i);
void* ui_viewports_main_hwnd(void);
int ui_viewports_present_mode(int i); // (its swapchain's VkPresentModeKHR)
int ui_viewports_wanted_present(void); // (the VkPresentModeKHR asked for)
