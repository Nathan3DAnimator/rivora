// (Q25b) Mixed reality: the Meta extensions xr_mr.c calls when the runtime has them, declared from the specification
// (layouts and values checked against the Khronos openxr.h of 2026) so the build needs no SDK. After xr_openxr.h.
#pragma once
#include "xr_openxr.h"

typedef XrFlags64 XrCompositionLayerFlags;
#define XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT 0x00000004ULL
typedef struct XrColor4f { float r, g, b, a; } XrColor4f;
typedef struct XrOffset2Df { float x, y; } XrOffset2Df;
typedef struct XrRect2Df { XrOffset2Df offset; XrExtent2Df extent; } XrRect2Df;
typedef struct XrExtent3Df { float width, height, depth; } XrExtent3Df;
typedef struct XrOffset3DfFB { float x, y, z; } XrOffset3DfFB;
typedef struct XrRect3DfFB { XrOffset3DfFB offset; XrExtent3Df extent; } XrRect3DfFB;
typedef struct XrUuidEXT { uint8_t data[16]; } XrUuidEXT;
typedef uint64_t XrAsyncRequestIdFB;
typedef XrResult (*PFN_xrEnumerateEnvironmentBlendModes)(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t, uint32_t*, XrEnvironmentBlendMode*);

// --- XR_FB_passthrough (spec version 5) ---
XR_DEFINE_HANDLE(XrPassthroughFB)
XR_DEFINE_HANDLE(XrPassthroughLayerFB)
#define XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB ((XrStructureType)1000118000)
#define XR_TYPE_PASSTHROUGH_CREATE_INFO_FB ((XrStructureType)1000118001)
#define XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB ((XrStructureType)1000118002)
#define XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB ((XrStructureType)1000118003)
#define XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB ((XrStructureType)1000118006)
#define XR_TYPE_PASSTHROUGH_STYLE_FB ((XrStructureType)1000118020)
#define XR_TYPE_PASSTHROUGH_COLOR_MAP_MONO_TO_RGBA_FB ((XrStructureType)1000118021)
#define XR_TYPE_PASSTHROUGH_COLOR_MAP_MONO_TO_MONO_FB ((XrStructureType)1000118022)
#define XR_TYPE_PASSTHROUGH_BRIGHTNESS_CONTRAST_SATURATION_FB ((XrStructureType)1000118023)
#define XR_TYPE_EVENT_DATA_PASSTHROUGH_STATE_CHANGED_FB ((XrStructureType)1000118030)
#define XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB 0
#define XR_PASSTHROUGH_CAPABILITY_BIT_FB 0x1ULL
#define XR_PASSTHROUGH_CAPABILITY_COLOR_BIT_FB 0x2ULL
#define XR_PASSTHROUGH_CAPABILITY_LAYER_DEPTH_BIT_FB 0x4ULL
#define XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB 0x1ULL
#define XR_PASSTHROUGH_STATE_CHANGED_REINIT_REQUIRED_BIT_FB 0x1ULL
#define XR_PASSTHROUGH_STATE_CHANGED_NON_RECOVERABLE_ERROR_BIT_FB 0x2ULL
#define XR_PASSTHROUGH_STATE_CHANGED_RECOVERABLE_ERROR_BIT_FB 0x4ULL
#define XR_PASSTHROUGH_STATE_CHANGED_RESTORED_ERROR_BIT_FB 0x8ULL
typedef struct XrSystemPassthroughPropertiesFB { XrStructureType type; const void* next; XrBool32 supportsPassthrough; } XrSystemPassthroughPropertiesFB;
typedef struct XrSystemPassthroughProperties2FB { XrStructureType type; const void* next; XrFlags64 capabilities; } XrSystemPassthroughProperties2FB;
typedef struct XrPassthroughCreateInfoFB { XrStructureType type; const void* next; XrFlags64 flags; } XrPassthroughCreateInfoFB;
typedef struct XrPassthroughLayerCreateInfoFB {
    XrStructureType type; const void* next; XrPassthroughFB passthrough; XrFlags64 flags; int32_t purpose;
} XrPassthroughLayerCreateInfoFB;
typedef struct XrCompositionLayerPassthroughFB {
    XrStructureType type; const void* next; XrCompositionLayerFlags flags; XrSpace space; XrPassthroughLayerFB layerHandle;
} XrCompositionLayerPassthroughFB;
typedef struct XrPassthroughStyleFB { XrStructureType type; const void* next; float textureOpacityFactor; XrColor4f edgeColor; } XrPassthroughStyleFB;
typedef struct XrPassthroughColorMapMonoToRgbaFB { XrStructureType type; const void* next; XrColor4f textureColorMap[256]; } XrPassthroughColorMapMonoToRgbaFB;
typedef struct XrPassthroughColorMapMonoToMonoFB { XrStructureType type; const void* next; uint8_t textureColorMap[256]; } XrPassthroughColorMapMonoToMonoFB;
typedef struct XrPassthroughBrightnessContrastSaturationFB {
    XrStructureType type; const void* next; float brightness, contrast, saturation;
} XrPassthroughBrightnessContrastSaturationFB;
typedef struct XrEventDataPassthroughStateChangedFB { XrStructureType type; const void* next; XrFlags64 flags; } XrEventDataPassthroughStateChangedFB;
typedef XrResult (*PFN_xrCreatePassthroughFB)(XrSession, const XrPassthroughCreateInfoFB*, XrPassthroughFB*);
typedef XrResult (*PFN_xrDestroyPassthroughFB)(XrPassthroughFB);
typedef XrResult (*PFN_xrPassthroughStartFB)(XrPassthroughFB);
typedef XrResult (*PFN_xrPassthroughPauseFB)(XrPassthroughFB);
typedef XrResult (*PFN_xrCreatePassthroughLayerFB)(XrSession, const XrPassthroughLayerCreateInfoFB*, XrPassthroughLayerFB*);
typedef XrResult (*PFN_xrDestroyPassthroughLayerFB)(XrPassthroughLayerFB);
typedef XrResult (*PFN_xrPassthroughLayerPauseFB)(XrPassthroughLayerFB);
typedef XrResult (*PFN_xrPassthroughLayerResumeFB)(XrPassthroughLayerFB);
typedef XrResult (*PFN_xrPassthroughLayerSetStyleFB)(XrPassthroughLayerFB, const XrPassthroughStyleFB*);

// --- XR_FB_spatial_entity, _query, _container, XR_FB_scene, XR_FB_scene_capture, XR_META_spatial_entity_mesh ---
#define XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB ((XrStructureType)1000113007)
#define XR_TYPE_SPACE_COMPONENT_STATUS_FB ((XrStructureType)1000113001)
#define XR_TYPE_EVENT_DATA_SPACE_SET_STATUS_COMPLETE_FB ((XrStructureType)1000113006)
#define XR_TYPE_SPACE_QUERY_INFO_FB ((XrStructureType)1000156001)
#define XR_TYPE_SPACE_QUERY_RESULTS_FB ((XrStructureType)1000156002)
#define XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB ((XrStructureType)1000156052)
#define XR_TYPE_SPACE_UUID_FILTER_INFO_FB ((XrStructureType)1000156054)
#define XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB ((XrStructureType)1000156003)
#define XR_SPACE_STORAGE_LOCATION_LOCAL_FB 1
#define XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB ((XrStructureType)1000156103)
#define XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB ((XrStructureType)1000156104)
#define XR_TYPE_SEMANTIC_LABELS_FB ((XrStructureType)1000175000)
#define XR_TYPE_BOUNDARY_2D_FB ((XrStructureType)1000175002)
#define XR_TYPE_SEMANTIC_LABELS_SUPPORT_INFO_FB ((XrStructureType)1000175010)
#define XR_TYPE_EVENT_DATA_SCENE_CAPTURE_COMPLETE_FB ((XrStructureType)1000198001)
#define XR_TYPE_SCENE_CAPTURE_REQUEST_INFO_FB ((XrStructureType)1000198050)
#define XR_TYPE_SPACE_CONTAINER_FB ((XrStructureType)1000199000)
#define XR_TYPE_SPACE_TRIANGLE_MESH_GET_INFO_META ((XrStructureType)1000269001)
#define XR_TYPE_SPACE_TRIANGLE_MESH_META ((XrStructureType)1000269002)
typedef enum XrSpaceComponentTypeFB {
    XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB = 0, XR_SPACE_COMPONENT_TYPE_STORABLE_FB = 1, XR_SPACE_COMPONENT_TYPE_SHARABLE_FB = 2,
    XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB = 3, XR_SPACE_COMPONENT_TYPE_BOUNDED_3D_FB = 4, XR_SPACE_COMPONENT_TYPE_SEMANTIC_LABELS_FB = 5,
    XR_SPACE_COMPONENT_TYPE_ROOM_LAYOUT_FB = 6, XR_SPACE_COMPONENT_TYPE_SPACE_CONTAINER_FB = 7,
    XR_SPACE_COMPONENT_TYPE_TRIANGLE_MESH_META = 1000269000, XR_SPACE_COMPONENT_TYPE_MAX_ENUM_FB = 0x7FFFFFFF
} XrSpaceComponentTypeFB;
#define XR_SPACE_QUERY_ACTION_LOAD_FB 0
#define XR_SEMANTIC_LABELS_SUPPORT_MULTIPLE_SEMANTIC_LABELS_BIT_FB 0x1ULL
#define XR_SEMANTIC_LABELS_SUPPORT_ACCEPT_DESK_TO_TABLE_MIGRATION_BIT_FB 0x2ULL
#define XR_SEMANTIC_LABELS_SUPPORT_ACCEPT_INVISIBLE_WALL_FACE_BIT_FB 0x4ULL
typedef struct XrSpaceComponentStatusSetInfoFB {
    XrStructureType type; const void* next; XrSpaceComponentTypeFB componentType; XrBool32 enabled; XrDuration timeout;
} XrSpaceComponentStatusSetInfoFB;
typedef struct XrSpaceComponentStatusFB { XrStructureType type; void* next; XrBool32 enabled, changePending; } XrSpaceComponentStatusFB;
typedef struct XrEventDataSpaceSetStatusCompleteFB {
    XrStructureType type; const void* next; XrAsyncRequestIdFB requestId; XrResult result; XrSpace space; XrUuidEXT uuid;
    XrSpaceComponentTypeFB componentType; XrBool32 enabled;
} XrEventDataSpaceSetStatusCompleteFB;
typedef struct XrSpaceFilterInfoBaseHeaderFB { XrStructureType type; const void* next; } XrSpaceFilterInfoBaseHeaderFB;
typedef struct XrSpaceQueryInfoFB {
    XrStructureType type; const void* next; int32_t queryAction; uint32_t maxResultCount; XrDuration timeout;
    const XrSpaceFilterInfoBaseHeaderFB* filter; const XrSpaceFilterInfoBaseHeaderFB* excludeFilter;
} XrSpaceQueryInfoFB;
typedef struct XrSpaceStorageLocationFilterInfoFB { XrStructureType type; const void* next; int32_t location; } XrSpaceStorageLocationFilterInfoFB;
typedef struct XrSpaceComponentFilterInfoFB { XrStructureType type; const void* next; XrSpaceComponentTypeFB componentType; } XrSpaceComponentFilterInfoFB;
typedef struct XrSpaceUuidFilterInfoFB { XrStructureType type; const void* next; uint32_t uuidCount; XrUuidEXT* uuids; } XrSpaceUuidFilterInfoFB;
typedef struct XrSpaceQueryResultFB { XrSpace space; XrUuidEXT uuid; } XrSpaceQueryResultFB;
typedef struct XrSpaceQueryResultsFB {
    XrStructureType type; void* next; uint32_t resultCapacityInput, resultCountOutput; XrSpaceQueryResultFB* results;
} XrSpaceQueryResultsFB;
typedef struct XrEventDataSpaceQueryResultsAvailableFB { XrStructureType type; const void* next; XrAsyncRequestIdFB requestId; } XrEventDataSpaceQueryResultsAvailableFB;
typedef struct XrEventDataSpaceQueryCompleteFB { XrStructureType type; const void* next; XrAsyncRequestIdFB requestId; XrResult result; } XrEventDataSpaceQueryCompleteFB;
typedef struct XrSemanticLabelsFB { XrStructureType type; const void* next; uint32_t bufferCapacityInput, bufferCountOutput; char* buffer; } XrSemanticLabelsFB;
typedef struct XrSemanticLabelsSupportInfoFB { XrStructureType type; const void* next; XrFlags64 flags; const char* recognizedLabels; } XrSemanticLabelsSupportInfoFB;
typedef struct XrBoundary2DFB { XrStructureType type; const void* next; uint32_t vertexCapacityInput, vertexCountOutput; XrVector2f* vertices; } XrBoundary2DFB;
typedef struct XrSpaceContainerFB { XrStructureType type; const void* next; uint32_t uuidCapacityInput, uuidCountOutput; XrUuidEXT* uuids; } XrSpaceContainerFB;
typedef struct XrSceneCaptureRequestInfoFB { XrStructureType type; const void* next; uint32_t requestByteCount; const char* request; } XrSceneCaptureRequestInfoFB;
typedef struct XrEventDataSceneCaptureCompleteFB { XrStructureType type; const void* next; XrAsyncRequestIdFB requestId; XrResult result; } XrEventDataSceneCaptureCompleteFB;
typedef struct XrSpaceTriangleMeshGetInfoMETA { XrStructureType type; const void* next; } XrSpaceTriangleMeshGetInfoMETA;
typedef struct XrSpaceTriangleMeshMETA {
    XrStructureType type; void* next; uint32_t vertexCapacityInput, vertexCountOutput; XrVector3f* vertices;
    uint32_t indexCapacityInput, indexCountOutput; uint32_t* indices;
} XrSpaceTriangleMeshMETA;
typedef XrResult (*PFN_xrEnumerateSpaceSupportedComponentsFB)(XrSpace, uint32_t, uint32_t*, XrSpaceComponentTypeFB*);
typedef XrResult (*PFN_xrSetSpaceComponentStatusFB)(XrSpace, const XrSpaceComponentStatusSetInfoFB*, XrAsyncRequestIdFB*);
typedef XrResult (*PFN_xrGetSpaceComponentStatusFB)(XrSpace, XrSpaceComponentTypeFB, XrSpaceComponentStatusFB*);
typedef XrResult (*PFN_xrQuerySpacesFB)(XrSession, const XrSpaceQueryInfoFB*, XrAsyncRequestIdFB*);
typedef XrResult (*PFN_xrRetrieveSpaceQueryResultsFB)(XrSession, XrAsyncRequestIdFB, XrSpaceQueryResultsFB*);
typedef XrResult (*PFN_xrGetSpaceBoundingBox2DFB)(XrSession, XrSpace, XrRect2Df*);
typedef XrResult (*PFN_xrGetSpaceBoundingBox3DFB)(XrSession, XrSpace, XrRect3DfFB*);
typedef XrResult (*PFN_xrGetSpaceSemanticLabelsFB)(XrSession, XrSpace, XrSemanticLabelsFB*);
typedef XrResult (*PFN_xrGetSpaceBoundary2DFB)(XrSession, XrSpace, XrBoundary2DFB*);
typedef XrResult (*PFN_xrGetSpaceContainerFB)(XrSession, XrSpace, XrSpaceContainerFB*);
typedef XrResult (*PFN_xrRequestSceneCaptureFB)(XrSession, const XrSceneCaptureRequestInfoFB*, XrAsyncRequestIdFB*);
typedef XrResult (*PFN_xrGetSpaceTriangleMeshMETA)(XrSpace, const XrSpaceTriangleMeshGetInfoMETA*, XrSpaceTriangleMeshMETA*);
typedef XrResult (*PFN_xrDestroySpace)(XrSpace);

// --- XR_META_environment_depth (spec version 2) ---
XR_DEFINE_HANDLE(XrEnvironmentDepthProviderMETA)
XR_DEFINE_HANDLE(XrEnvironmentDepthSwapchainMETA)
#define XR_TYPE_ENVIRONMENT_DEPTH_PROVIDER_CREATE_INFO_META ((XrStructureType)1000291000)
#define XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_CREATE_INFO_META ((XrStructureType)1000291001)
#define XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_STATE_META ((XrStructureType)1000291002)
#define XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_ACQUIRE_INFO_META ((XrStructureType)1000291003)
#define XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META ((XrStructureType)1000291004)
#define XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_META ((XrStructureType)1000291005)
#define XR_TYPE_ENVIRONMENT_DEPTH_HAND_REMOVAL_SET_INFO_META ((XrStructureType)1000291006)
#define XR_TYPE_SYSTEM_ENVIRONMENT_DEPTH_PROPERTIES_META ((XrStructureType)1000291007)
#define XR_ENVIRONMENT_DEPTH_NOT_AVAILABLE_META 1000291000 // (a success code: no new depth yet)
typedef struct XrEnvironmentDepthProviderCreateInfoMETA { XrStructureType type; const void* next; XrFlags64 createFlags; } XrEnvironmentDepthProviderCreateInfoMETA;
typedef struct XrEnvironmentDepthSwapchainCreateInfoMETA { XrStructureType type; const void* next; XrFlags64 createFlags; } XrEnvironmentDepthSwapchainCreateInfoMETA;
typedef struct XrEnvironmentDepthSwapchainStateMETA { XrStructureType type; void* next; uint32_t width, height; } XrEnvironmentDepthSwapchainStateMETA;
typedef struct XrEnvironmentDepthImageAcquireInfoMETA { XrStructureType type; const void* next; XrSpace space; XrTime displayTime; } XrEnvironmentDepthImageAcquireInfoMETA;
typedef struct XrEnvironmentDepthImageViewMETA { XrStructureType type; const void* next; XrFovf fov; XrPosef pose; } XrEnvironmentDepthImageViewMETA;
typedef struct XrEnvironmentDepthImageMETA {
    XrStructureType type; const void* next; uint32_t swapchainIndex; float nearZ, farZ; XrEnvironmentDepthImageViewMETA views[2];
} XrEnvironmentDepthImageMETA;
typedef struct XrEnvironmentDepthHandRemovalSetInfoMETA { XrStructureType type; const void* next; XrBool32 enabled; } XrEnvironmentDepthHandRemovalSetInfoMETA;
typedef struct XrSystemEnvironmentDepthPropertiesMETA {
    XrStructureType type; void* next; XrBool32 supportsEnvironmentDepth, supportsHandRemoval;
} XrSystemEnvironmentDepthPropertiesMETA;
typedef XrResult (*PFN_xrCreateEnvironmentDepthProviderMETA)(XrSession, const XrEnvironmentDepthProviderCreateInfoMETA*, XrEnvironmentDepthProviderMETA*);
typedef XrResult (*PFN_xrDestroyEnvironmentDepthProviderMETA)(XrEnvironmentDepthProviderMETA);
typedef XrResult (*PFN_xrStartEnvironmentDepthProviderMETA)(XrEnvironmentDepthProviderMETA);
typedef XrResult (*PFN_xrStopEnvironmentDepthProviderMETA)(XrEnvironmentDepthProviderMETA);
typedef XrResult (*PFN_xrCreateEnvironmentDepthSwapchainMETA)(XrEnvironmentDepthProviderMETA, const XrEnvironmentDepthSwapchainCreateInfoMETA*, XrEnvironmentDepthSwapchainMETA*);
typedef XrResult (*PFN_xrDestroyEnvironmentDepthSwapchainMETA)(XrEnvironmentDepthSwapchainMETA);
typedef XrResult (*PFN_xrEnumerateEnvironmentDepthSwapchainImagesMETA)(XrEnvironmentDepthSwapchainMETA, uint32_t, uint32_t*, XrSwapchainImageBaseHeader*);
typedef XrResult (*PFN_xrGetEnvironmentDepthSwapchainStateMETA)(XrEnvironmentDepthSwapchainMETA, XrEnvironmentDepthSwapchainStateMETA*);
typedef XrResult (*PFN_xrAcquireEnvironmentDepthImageMETA)(XrEnvironmentDepthProviderMETA, const XrEnvironmentDepthImageAcquireInfoMETA*, XrEnvironmentDepthImageMETA*);
typedef XrResult (*PFN_xrSetEnvironmentDepthHandRemovalMETA)(XrEnvironmentDepthProviderMETA, const XrEnvironmentDepthHandRemovalSetInfoMETA*);
