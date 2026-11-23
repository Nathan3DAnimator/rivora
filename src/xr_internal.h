// (Q25b) xr.c's session as xr_mr.c sees it, and the hooks xr.c calls (mixed reality: passthrough, the room, the real depth)
#pragma once
#include <stdbool.h>

#include "xr_openxr.h"

typedef struct XrInternal {
    XrInstance instance;
    XrSystemId system;
    XrSession session;
    XrSpace space;          // the play space (STAGE or LOCAL)
    XrTime display_time;    // this frame's
    PFN_xrGetInstanceProcAddr gipa;
    VkDevice device;
    bool running, sim;
} XrInternal;
void xr_internal(XrInternal* out);

// xr_mr.c, called by xr.c: the extensions it wants among the runtime's (added to names, *n of cap); the instance and the
// system made (its functions, its properties); the session started; each event; each frame after the views are located;
// the layers to submit (the passthrough's under the projection: its flags, the blend mode); the session ending
void xr_mr_pick_extensions(const void* props, uint32_t nprops, const char** names, uint32_t* n, uint32_t cap);
void xr_mr_system_ready(void);
void xr_mr_session_ready(void);
void xr_mr_event(const void* event_buffer);
void xr_mr_frame(void);
const void* xr_mr_compose(XrFlags64* projection_flags, int* blend_mode); // the passthrough layer to put first (NULL: none)
void xr_mr_shutdown(void);
