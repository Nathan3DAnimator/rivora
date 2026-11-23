// Compilers other than MSVC (clang of the Android NDK: the Android game; gcc/clang on Linux): the MSVC names the code
// uses, mapped to their POSIX equivalents. Force-included (-include) by CMakeLists.txt when the compiler is not MSVC.
#pragma once
#if !defined(_MSC_VER)
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define strtok_s strtok_r
#define _strdup strdup

static inline int re_strcpy_s_(char* dst, size_t cap, const char* src) {
    if (!dst || !cap) return 22;
    snprintf(dst, cap, "%s", src ? src : "");
    return 0;
}
#define strcpy_s re_strcpy_s_

// __declspec(thread), __declspec(align(n)), __declspec(noinline), __declspec(noreturn)
#define __declspec(x) RE_DECLSPEC_##x
#define RE_DECLSPEC_thread __thread
#define RE_DECLSPEC_noinline __attribute__((noinline))
#define RE_DECLSPEC_noreturn __attribute__((noreturn))
#define RE_DECLSPEC_dllimport
#define RE_DECLSPEC_dllexport __attribute__((visibility("default")))
#define RE_DECLSPEC_align(n) __attribute__((aligned(n)))
#endif

#if defined(__ANDROID__) && !defined(RE_NO_VK_RENAME)
// Android's libvulkan exports the Vulkan 1.1 core only: the 1.2 functions the app calls directly come through
// vkGetDeviceProcAddr (vkctx.c). Defined before <vulkan/vulkan.h> is read, so its prototype is renamed too.
#define vkGetBufferDeviceAddress re_vkGetBufferDeviceAddress
#define vkCmdBeginRendering re_vkCmdBeginRendering
#define vkCmdEndRendering re_vkCmdEndRendering
#define vkCmdPipelineBarrier2 re_vkCmdPipelineBarrier2
#define vkCmdWriteTimestamp2 re_vkCmdWriteTimestamp2
#define vkQueueSubmit2 re_vkQueueSubmit2
#define vkWaitSemaphores re_vkWaitSemaphores
#define vkCmdDrawIndexedIndirectCount re_vkCmdDrawIndexedIndirectCount
#endif
