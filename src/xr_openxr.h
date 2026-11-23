// The part of OpenXR 1.0 (Khronos, Apache-2.0 API) the engine calls, declared here from the specification so the
// build needs no SDK; the runtime is loaded at run time (xr.c). Only what xr.c uses; layouts as openxr.h's.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#define XR_DEFINE_HANDLE(o) typedef struct o##_T* o;
XR_DEFINE_HANDLE(XrInstance)
XR_DEFINE_HANDLE(XrSession)
XR_DEFINE_HANDLE(XrSpace)
XR_DEFINE_HANDLE(XrSwapchain)
XR_DEFINE_HANDLE(XrActionSet)
XR_DEFINE_HANDLE(XrAction)
typedef uint64_t XrVersion, XrFlags64, XrSystemId, XrPath;
typedef int64_t XrTime, XrDuration;
typedef uint32_t XrBool32;
typedef int32_t XrResult;
#define XR_SUCCEEDED(r) ((r) >= 0)
#define XR_MAKE_VERSION(maj, min, pat) ((((uint64_t)(maj) & 0xffffULL) << 48) | (((uint64_t)(min) & 0xffffULL) << 32) | ((pat) & 0xffffffffULL))
#define XR_INFINITE_DURATION 0x7fffffffffffffffLL
#define XR_NULL_PATH 0
#define XR_SESSION_LOSS_PENDING 3
#define XR_EVENT_UNAVAILABLE 4

typedef enum XrStructureType {
    XR_TYPE_INSTANCE_CREATE_INFO = 3, XR_TYPE_SYSTEM_GET_INFO = 4, XR_TYPE_SYSTEM_PROPERTIES = 5, XR_TYPE_VIEW_LOCATE_INFO = 6,
    XR_TYPE_VIEW = 7, XR_TYPE_SESSION_CREATE_INFO = 8, XR_TYPE_SWAPCHAIN_CREATE_INFO = 9, XR_TYPE_SESSION_BEGIN_INFO = 10,
    XR_TYPE_VIEW_STATE = 11, XR_TYPE_FRAME_END_INFO = 12, XR_TYPE_HAPTIC_VIBRATION = 13, XR_TYPE_EVENT_DATA_BUFFER = 16,
    XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED = 18, XR_TYPE_ACTION_STATE_BOOLEAN = 23, XR_TYPE_ACTION_STATE_FLOAT = 24,
    XR_TYPE_ACTION_STATE_VECTOR2F = 25, XR_TYPE_ACTION_STATE_POSE = 27, XR_TYPE_ACTION_SET_CREATE_INFO = 28,
    XR_TYPE_ACTION_CREATE_INFO = 29, XR_TYPE_INSTANCE_PROPERTIES = 32, XR_TYPE_FRAME_WAIT_INFO = 33,
    XR_TYPE_COMPOSITION_LAYER_PROJECTION = 35, XR_TYPE_REFERENCE_SPACE_CREATE_INFO = 37, XR_TYPE_ACTION_SPACE_CREATE_INFO = 38,
    XR_TYPE_VIEW_CONFIGURATION_VIEW = 41, XR_TYPE_SPACE_LOCATION = 42, XR_TYPE_FRAME_STATE = 44, XR_TYPE_FRAME_BEGIN_INFO = 46,
    XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW = 48, XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING = 51,
    XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO = 55, XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO = 56, XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO = 57,
    XR_TYPE_ACTION_STATE_GET_INFO = 58, XR_TYPE_HAPTIC_ACTION_INFO = 59, XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO = 60,
    XR_TYPE_ACTIONS_SYNC_INFO = 61,
    XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR = 1000025000, XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR = 1000025001,
    XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR = 1000025002,
    XR_STRUCTURE_TYPE_MAX_ENUM = 0x7FFFFFFF
} XrStructureType;
typedef enum XrFormFactor { XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY = 1, XR_FORM_FACTOR_MAX_ENUM = 0x7FFFFFFF } XrFormFactor;
typedef enum XrViewConfigurationType { XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO = 2, XR_VCT_MAX_ENUM = 0x7FFFFFFF } XrViewConfigurationType;
typedef enum XrReferenceSpaceType { XR_REFERENCE_SPACE_TYPE_VIEW = 1, XR_REFERENCE_SPACE_TYPE_LOCAL = 2, XR_REFERENCE_SPACE_TYPE_STAGE = 3, XR_RST_MAX_ENUM = 0x7FFFFFFF } XrReferenceSpaceType;
typedef enum XrEnvironmentBlendMode { XR_ENVIRONMENT_BLEND_MODE_OPAQUE = 1, XR_ENVIRONMENT_BLEND_MODE_ADDITIVE = 2, XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND = 3, XR_EBM_MAX_ENUM = 0x7FFFFFFF } XrEnvironmentBlendMode;
typedef enum XrSessionState { XR_SESSION_STATE_UNKNOWN = 0, XR_SESSION_STATE_IDLE = 1, XR_SESSION_STATE_READY = 2, XR_SESSION_STATE_SYNCHRONIZED = 3,
                              XR_SESSION_STATE_VISIBLE = 4, XR_SESSION_STATE_FOCUSED = 5, XR_SESSION_STATE_STOPPING = 6,
                              XR_SESSION_STATE_LOSS_PENDING = 7, XR_SESSION_STATE_EXITING = 8, XR_SS_MAX_ENUM = 0x7FFFFFFF } XrSessionState;
typedef enum XrActionType { XR_ACTION_TYPE_BOOLEAN_INPUT = 1, XR_ACTION_TYPE_FLOAT_INPUT = 2, XR_ACTION_TYPE_VECTOR2F_INPUT = 3,
                            XR_ACTION_TYPE_POSE_INPUT = 4, XR_ACTION_TYPE_VIBRATION_OUTPUT = 100, XR_AT_MAX_ENUM = 0x7FFFFFFF } XrActionType;
#define XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT 0x00000001ULL
#define XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT 0x00000010ULL
#define XR_SWAPCHAIN_USAGE_SAMPLED_BIT 0x00000020ULL
#define XR_SPACE_LOCATION_ORIENTATION_VALID_BIT 0x00000001ULL
#define XR_SPACE_LOCATION_POSITION_VALID_BIT 0x00000002ULL

typedef struct XrVector2f { float x, y; } XrVector2f;
typedef struct XrVector3f { float x, y, z; } XrVector3f;
typedef struct XrQuaternionf { float x, y, z, w; } XrQuaternionf;
typedef struct XrPosef { XrQuaternionf orientation; XrVector3f position; } XrPosef;
typedef struct XrFovf { float angleLeft, angleRight, angleUp, angleDown; } XrFovf;
typedef struct XrOffset2Di { int32_t x, y; } XrOffset2Di;
typedef struct XrExtent2Di { int32_t width, height; } XrExtent2Di;
typedef struct XrRect2Di { XrOffset2Di offset; XrExtent2Di extent; } XrRect2Di;

typedef struct XrApplicationInfo {
    char applicationName[128];
    uint32_t applicationVersion;
    char engineName[128];
    uint32_t engineVersion;
    XrVersion apiVersion;
} XrApplicationInfo;
typedef struct XrInstanceCreateInfo {
    XrStructureType type; const void* next; XrFlags64 createFlags; XrApplicationInfo applicationInfo;
    uint32_t enabledApiLayerCount; const char* const* enabledApiLayerNames;
    uint32_t enabledExtensionCount; const char* const* enabledExtensionNames;
} XrInstanceCreateInfo;
typedef struct XrInstanceProperties { XrStructureType type; void* next; XrVersion runtimeVersion; char runtimeName[128]; } XrInstanceProperties;
typedef struct XrSystemGetInfo { XrStructureType type; const void* next; XrFormFactor formFactor; } XrSystemGetInfo;
typedef struct XrSystemGraphicsProperties { uint32_t maxSwapchainImageHeight, maxSwapchainImageWidth, maxLayerCount; } XrSystemGraphicsProperties;
typedef struct XrSystemTrackingProperties { XrBool32 orientationTracking, positionTracking; } XrSystemTrackingProperties;
typedef struct XrSystemProperties {
    XrStructureType type; void* next; XrSystemId systemId; uint32_t vendorId; char systemName[256];
    XrSystemGraphicsProperties graphicsProperties; XrSystemTrackingProperties trackingProperties;
} XrSystemProperties;
typedef struct XrViewConfigurationView {
    XrStructureType type; void* next;
    uint32_t recommendedImageRectWidth, maxImageRectWidth, recommendedImageRectHeight, maxImageRectHeight;
    uint32_t recommendedSwapchainSampleCount, maxSwapchainSampleCount;
} XrViewConfigurationView;
typedef struct XrGraphicsRequirementsVulkanKHR { XrStructureType type; void* next; XrVersion minApiVersionSupported, maxApiVersionSupported; } XrGraphicsRequirementsVulkanKHR;
typedef struct XrGraphicsBindingVulkanKHR {
    XrStructureType type; const void* next; VkInstance instance; VkPhysicalDevice physicalDevice; VkDevice device;
    uint32_t queueFamilyIndex, queueIndex;
} XrGraphicsBindingVulkanKHR;
typedef struct XrSessionCreateInfo { XrStructureType type; const void* next; XrFlags64 createFlags; XrSystemId systemId; } XrSessionCreateInfo;
typedef struct XrSessionBeginInfo { XrStructureType type; const void* next; XrViewConfigurationType primaryViewConfigurationType; } XrSessionBeginInfo;
typedef struct XrReferenceSpaceCreateInfo { XrStructureType type; const void* next; XrReferenceSpaceType referenceSpaceType; XrPosef poseInReferenceSpace; } XrReferenceSpaceCreateInfo;
typedef struct XrSwapchainCreateInfo {
    XrStructureType type; const void* next; XrFlags64 createFlags, usageFlags; int64_t format;
    uint32_t sampleCount, width, height, faceCount, arraySize, mipCount;
} XrSwapchainCreateInfo;
typedef struct XrSwapchainImageVulkanKHR { XrStructureType type; void* next; VkImage image; } XrSwapchainImageVulkanKHR;
typedef struct XrSwapchainImageBaseHeader { XrStructureType type; void* next; } XrSwapchainImageBaseHeader;
typedef struct XrSwapchainImageAcquireInfo { XrStructureType type; const void* next; } XrSwapchainImageAcquireInfo;
typedef struct XrSwapchainImageWaitInfo { XrStructureType type; const void* next; XrDuration timeout; } XrSwapchainImageWaitInfo;
typedef struct XrSwapchainImageReleaseInfo { XrStructureType type; const void* next; } XrSwapchainImageReleaseInfo;
typedef struct XrFrameWaitInfo { XrStructureType type; const void* next; } XrFrameWaitInfo;
typedef struct XrFrameState { XrStructureType type; void* next; XrTime predictedDisplayTime; XrDuration predictedDisplayPeriod; XrBool32 shouldRender; } XrFrameState;
typedef struct XrFrameBeginInfo { XrStructureType type; const void* next; } XrFrameBeginInfo;
typedef struct XrViewLocateInfo { XrStructureType type; const void* next; XrViewConfigurationType viewConfigurationType; XrTime displayTime; XrSpace space; } XrViewLocateInfo;
typedef struct XrViewState { XrStructureType type; void* next; XrFlags64 viewStateFlags; } XrViewState;
typedef struct XrView { XrStructureType type; void* next; XrPosef pose; XrFovf fov; } XrView;
typedef struct XrSwapchainSubImage { XrSwapchain swapchain; XrRect2Di imageRect; uint32_t imageArrayIndex; } XrSwapchainSubImage;
typedef struct XrCompositionLayerProjectionView { XrStructureType type; const void* next; XrPosef pose; XrFovf fov; XrSwapchainSubImage subImage; } XrCompositionLayerProjectionView;
typedef struct XrCompositionLayerBaseHeader { XrStructureType type; const void* next; XrFlags64 layerFlags; XrSpace space; } XrCompositionLayerBaseHeader;
typedef struct XrCompositionLayerProjection {
    XrStructureType type; const void* next; XrFlags64 layerFlags; XrSpace space; uint32_t viewCount; const XrCompositionLayerProjectionView* views;
} XrCompositionLayerProjection;
typedef struct XrFrameEndInfo {
    XrStructureType type; const void* next; XrTime displayTime; XrEnvironmentBlendMode environmentBlendMode;
    uint32_t layerCount; const XrCompositionLayerBaseHeader* const* layers;
} XrFrameEndInfo;
typedef struct XrEventDataBuffer { XrStructureType type; const void* next; uint8_t varying[4000]; } XrEventDataBuffer;
typedef struct XrEventDataSessionStateChanged { XrStructureType type; const void* next; XrSession session; XrSessionState state; XrTime time; } XrEventDataSessionStateChanged;
typedef struct XrActionSetCreateInfo { XrStructureType type; const void* next; char actionSetName[64]; char localizedActionSetName[128]; uint32_t priority; } XrActionSetCreateInfo;
typedef struct XrActionCreateInfo {
    XrStructureType type; const void* next; char actionName[64]; XrActionType actionType;
    uint32_t countSubactionPaths; const XrPath* subactionPaths; char localizedActionName[128];
} XrActionCreateInfo;
typedef struct XrActionSuggestedBinding { XrAction action; XrPath binding; } XrActionSuggestedBinding;
typedef struct XrInteractionProfileSuggestedBinding {
    XrStructureType type; const void* next; XrPath interactionProfile; uint32_t countSuggestedBindings; const XrActionSuggestedBinding* suggestedBindings;
} XrInteractionProfileSuggestedBinding;
typedef struct XrSessionActionSetsAttachInfo { XrStructureType type; const void* next; uint32_t countActionSets; const XrActionSet* actionSets; } XrSessionActionSetsAttachInfo;
typedef struct XrActionSpaceCreateInfo { XrStructureType type; const void* next; XrAction action; XrPath subactionPath; XrPosef poseInActionSpace; } XrActionSpaceCreateInfo;
typedef struct XrActiveActionSet { XrActionSet actionSet; XrPath subactionPath; } XrActiveActionSet;
typedef struct XrActionsSyncInfo { XrStructureType type; const void* next; uint32_t countActiveActionSets; const XrActiveActionSet* activeActionSets; } XrActionsSyncInfo;
typedef struct XrActionStateGetInfo { XrStructureType type; const void* next; XrAction action; XrPath subactionPath; } XrActionStateGetInfo;
typedef struct XrActionStateBoolean { XrStructureType type; void* next; XrBool32 currentState, changedSinceLastSync; XrTime lastChangeTime; XrBool32 isActive; } XrActionStateBoolean;
typedef struct XrActionStateFloat { XrStructureType type; void* next; float currentState; XrBool32 changedSinceLastSync; XrTime lastChangeTime; XrBool32 isActive; } XrActionStateFloat;
typedef struct XrActionStateVector2f { XrStructureType type; void* next; XrVector2f currentState; XrBool32 changedSinceLastSync; XrTime lastChangeTime; XrBool32 isActive; } XrActionStateVector2f;
typedef struct XrSpaceLocation { XrStructureType type; void* next; XrFlags64 locationFlags; XrPosef pose; } XrSpaceLocation;
typedef struct XrHapticActionInfo { XrStructureType type; const void* next; XrAction action; XrPath subactionPath; } XrHapticActionInfo;
typedef struct XrHapticVibration { XrStructureType type; const void* next; XrDuration duration; float frequency; float amplitude; } XrHapticVibration;
typedef struct XrHapticBaseHeader { XrStructureType type; const void* next; } XrHapticBaseHeader;

typedef XrResult (*PFN_xrVoidFunction)(void);
typedef XrResult (*PFN_xrGetInstanceProcAddr)(XrInstance instance, const char* name, PFN_xrVoidFunction* function);

// The loader <-> runtime negotiation (the loader's interface, openxr_loader_negotiation.h)
typedef enum XrLoaderInterfaceStructs { XR_LOADER_INTERFACE_STRUCT_LOADER_INFO = 1, XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST = 3, XR_LIS_MAX = 0x7FFFFFFF } XrLoaderInterfaceStructs;
typedef struct XrNegotiateLoaderInfo {
    XrLoaderInterfaceStructs structType; uint32_t structVersion; size_t structSize;
    uint32_t minInterfaceVersion, maxInterfaceVersion; XrVersion minApiVersion, maxApiVersion;
} XrNegotiateLoaderInfo;
typedef struct XrNegotiateRuntimeRequest {
    XrLoaderInterfaceStructs structType; uint32_t structVersion; size_t structSize;
    uint32_t runtimeInterfaceVersion; XrVersion runtimeApiVersion; PFN_xrGetInstanceProcAddr getInstanceProcAddr;
} XrNegotiateRuntimeRequest;
typedef XrResult (*PFN_xrNegotiateLoaderRuntimeInterface)(const XrNegotiateLoaderInfo* loaderInfo, XrNegotiateRuntimeRequest* runtimeRequest);

// (Q25) The extensions xr.c uses when the runtime has them (the specification's layouts and values)
#define XR_TYPE_EXTENSION_PROPERTIES ((XrStructureType)2)
#define XR_TYPE_COMPOSITION_LAYER_QUAD ((XrStructureType)36)
#define XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR ((XrStructureType)1000008000)
#define XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR ((XrStructureType)1000089000)
#define XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT ((XrStructureType)1000051000)
#define XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT ((XrStructureType)1000051001)
#define XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT ((XrStructureType)1000051002)
#define XR_TYPE_HAND_JOINT_LOCATIONS_EXT ((XrStructureType)1000051003)
#define XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB ((XrStructureType)1000101000)
#define XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT 0x00000002ULL
#define XR_HAND_JOINT_COUNT_EXT 26
XR_DEFINE_HANDLE(XrHandTrackerEXT)
typedef struct XrExtensionProperties { XrStructureType type; void* next; char extensionName[128]; uint32_t extensionVersion; } XrExtensionProperties;
typedef XrResult (*PFN_xrEnumerateInstanceExtensionProperties)(const char*, uint32_t, uint32_t*, XrExtensionProperties*);
// XR_KHR_loader_init_android, XR_KHR_android_create_instance (the Quest: the Khronos loader in the APK)
typedef struct XrLoaderInitInfoBaseHeaderKHR { XrStructureType type; const void* next; } XrLoaderInitInfoBaseHeaderKHR;
typedef struct XrLoaderInitInfoAndroidKHR { XrStructureType type; const void* next; void* applicationVM; void* applicationContext; } XrLoaderInitInfoAndroidKHR;
typedef XrResult (*PFN_xrInitializeLoaderKHR)(const XrLoaderInitInfoBaseHeaderKHR*);
typedef struct XrInstanceCreateInfoAndroidKHR { XrStructureType type; const void* next; void* applicationVM; void* applicationActivity; } XrInstanceCreateInfoAndroidKHR;
// XR_FB_display_refresh_rate
typedef XrResult (*PFN_xrEnumerateDisplayRefreshRatesFB)(XrSession, uint32_t, uint32_t*, float*);
typedef XrResult (*PFN_xrGetDisplayRefreshRateFB)(XrSession, float*);
typedef XrResult (*PFN_xrRequestDisplayRefreshRateFB)(XrSession, float);
typedef struct XrEventDataDisplayRefreshRateChangedFB { XrStructureType type; const void* next; float fromDisplayRefreshRate, toDisplayRefreshRate; } XrEventDataDisplayRefreshRateChangedFB;
// XR_EXT_hand_tracking
typedef enum XrHandEXT { XR_HAND_LEFT_EXT = 1, XR_HAND_RIGHT_EXT = 2, XR_HAND_MAX_ENUM_EXT = 0x7FFFFFFF } XrHandEXT;
typedef struct XrSystemHandTrackingPropertiesEXT { XrStructureType type; void* next; XrBool32 supportsHandTracking; } XrSystemHandTrackingPropertiesEXT;
typedef struct XrHandTrackerCreateInfoEXT { XrStructureType type; const void* next; XrHandEXT hand; int32_t handJointSet; } XrHandTrackerCreateInfoEXT;
typedef struct XrHandJointsLocateInfoEXT { XrStructureType type; const void* next; XrSpace baseSpace; XrTime time; } XrHandJointsLocateInfoEXT;
typedef struct XrHandJointLocationEXT { XrFlags64 locationFlags; XrPosef pose; float radius; } XrHandJointLocationEXT;
typedef struct XrHandJointLocationsEXT { XrStructureType type; void* next; XrBool32 isActive; uint32_t jointCount; XrHandJointLocationEXT* jointLocations; } XrHandJointLocationsEXT;
typedef XrResult (*PFN_xrCreateHandTrackerEXT)(XrSession, const XrHandTrackerCreateInfoEXT*, XrHandTrackerEXT*);
typedef XrResult (*PFN_xrDestroyHandTrackerEXT)(XrHandTrackerEXT);
typedef XrResult (*PFN_xrLocateHandJointsEXT)(XrHandTrackerEXT, const XrHandJointsLocateInfoEXT*, XrHandJointLocationsEXT*);
// a quad layer (the VR menus: the game's UI on a panel in the world, sharp at the headset's own resolution)
typedef struct XrExtent2Df { float width, height; } XrExtent2Df;
typedef struct XrCompositionLayerQuad {
    XrStructureType type; const void* next; XrFlags64 layerFlags; XrSpace space; int32_t eyeVisibility;
    XrSwapchainSubImage subImage; XrPosef pose; XrExtent2Df size;
} XrCompositionLayerQuad;
