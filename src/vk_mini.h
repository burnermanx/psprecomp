/* Minimal self-contained Vulkan 1.0 core header for psprecomp.
 * Matches the Khronos Vulkan 1.0 C ABI (vulkan_core.h) so the Vulkan backend
 * builds without requiring an external vulkan-headers package. */
#ifndef PSPRECOMP_VK_MINI_H
#define PSPRECOMP_VK_MINI_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define VKAPI_ATTR
#define VKAPI_CALL __stdcall
#define VKAPI_PTR  VKAPI_CALL
#else
#define VKAPI_ATTR
#define VKAPI_CALL
#define VKAPI_PTR
#endif

#define VK_MAKE_VERSION(major, minor, patch) \
    ((((uint32_t)(major)) << 22) | (((uint32_t)(minor)) << 12) | ((uint32_t)(patch)))
#define VK_API_VERSION_1_0 VK_MAKE_VERSION(1, 0, 0)

#define VK_TRUE  1U
#define VK_FALSE 0U
#define VK_NULL_HANDLE NULL
#define VK_QUEUE_FAMILY_IGNORED (~0U)
#define VK_SUBPASS_EXTERNAL     (~0U)
#define VK_MAX_PHYSICAL_DEVICE_NAME_SIZE 256U
#define VK_UUID_SIZE 16U
#define VK_MAX_MEMORY_TYPES 32U
#define VK_MAX_MEMORY_HEAPS 16U

typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;
typedef uint32_t VkSampleMask;

#define VK_DEFINE_HANDLE(obj) typedef struct obj##_T* obj;
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(obj) typedef struct obj##_T* obj;

VK_DEFINE_HANDLE(VkInstance)
VK_DEFINE_HANDLE(VkPhysicalDevice)
VK_DEFINE_HANDLE(VkDevice)
VK_DEFINE_HANDLE(VkQueue)
VK_DEFINE_HANDLE(VkCommandBuffer)

VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSemaphore)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkFence)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDeviceMemory)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkBuffer)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkImage)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkEvent)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkQueryPool)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkBufferView)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkImageView)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkShaderModule)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkPipelineCache)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkPipelineLayout)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkRenderPass)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkPipeline)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDescriptorSetLayout)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSampler)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDescriptorPool)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDescriptorSet)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkFramebuffer)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkCommandPool)

typedef enum VkResult {
    VK_SUCCESS = 0,
    VK_NOT_READY = 1,
    VK_TIMEOUT = 2,
    VK_INCOMPLETE = 5,
    VK_ERROR_OUT_OF_HOST_MEMORY = -1,
    VK_ERROR_OUT_OF_DEVICE_MEMORY = -2,
    VK_ERROR_INITIALIZATION_FAILED = -3,
    VK_ERROR_DEVICE_LOST = -4
} VkResult;

typedef enum VkStructureType {
    VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
    VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
    VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3,
    VK_STRUCTURE_TYPE_SUBMIT_INFO = 4,
    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5,
    VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8,
    VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO = 11,
    VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12,
    VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14,
    VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO = 15,
    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16,
    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 18,
    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO = 19,
    VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO = 20,
    VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO = 22,
    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO = 23,
    VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO = 24,
    VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO = 25,
    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO = 26,
    VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO = 27,
    VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO = 28,
    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO = 30,
    VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO = 31,
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32,
    VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO = 33,
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO = 34,
    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35,
    VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO = 37,
    VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO = 38,
    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42,
    VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO = 43,
    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER = 45,
    VK_STRUCTURE_TYPE_MEMORY_BARRIER = 46
} VkStructureType;

typedef enum VkPhysicalDeviceType {
    VK_PHYSICAL_DEVICE_TYPE_OTHER = 0,
    VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1,
    VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2,
    VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU = 3,
    VK_PHYSICAL_DEVICE_TYPE_CPU = 4
} VkPhysicalDeviceType;

typedef enum VkFormat {
    VK_FORMAT_UNDEFINED = 0,
    VK_FORMAT_R8G8B8A8_UNORM = 37,
    VK_FORMAT_R32G32_SFLOAT = 103,
    VK_FORMAT_R32G32B32_SFLOAT = 106,
    VK_FORMAT_D16_UNORM = 124
} VkFormat;

typedef enum VkImageType {
    VK_IMAGE_TYPE_1D = 0,
    VK_IMAGE_TYPE_2D = 1,
    VK_IMAGE_TYPE_3D = 2
} VkImageType;

typedef enum VkImageTiling {
    VK_IMAGE_TILING_OPTIMAL = 0,
    VK_IMAGE_TILING_LINEAR = 1
} VkImageTiling;

typedef enum VkImageViewType {
    VK_IMAGE_VIEW_TYPE_1D = 0,
    VK_IMAGE_VIEW_TYPE_2D = 1,
    VK_IMAGE_VIEW_TYPE_3D = 2
} VkImageViewType;

typedef enum VkSharingMode {
    VK_SHARING_MODE_EXCLUSIVE = 0,
    VK_SHARING_MODE_CONCURRENT = 1
} VkSharingMode;

typedef enum VkImageLayout {
    VK_IMAGE_LAYOUT_UNDEFINED = 0,
    VK_IMAGE_LAYOUT_GENERAL = 1,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2,
    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL = 3,
    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL = 4,
    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 5,
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6,
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 7,
    VK_IMAGE_LAYOUT_PREINITIALIZED = 8
} VkImageLayout;

typedef enum VkComponentSwizzle {
    VK_COMPONENT_SWIZZLE_IDENTITY = 0
} VkComponentSwizzle;

typedef enum VkAttachmentLoadOp {
    VK_ATTACHMENT_LOAD_OP_LOAD = 0,
    VK_ATTACHMENT_LOAD_OP_CLEAR = 1,
    VK_ATTACHMENT_LOAD_OP_DONT_CARE = 2
} VkAttachmentLoadOp;

typedef enum VkAttachmentStoreOp {
    VK_ATTACHMENT_STORE_OP_STORE = 0,
    VK_ATTACHMENT_STORE_OP_DONT_CARE = 1
} VkAttachmentStoreOp;

typedef enum VkPipelineBindPoint {
    VK_PIPELINE_BIND_POINT_GRAPHICS = 0,
    VK_PIPELINE_BIND_POINT_COMPUTE = 1
} VkPipelineBindPoint;

typedef enum VkCommandBufferLevel {
    VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0,
    VK_COMMAND_BUFFER_LEVEL_SECONDARY = 1
} VkCommandBufferLevel;

typedef enum VkSubpassContents {
    VK_SUBPASS_CONTENTS_INLINE = 0,
    VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS = 1
} VkSubpassContents;

typedef enum VkQueryType {
    VK_QUERY_TYPE_OCCLUSION = 0,
    VK_QUERY_TYPE_PIPELINE_STATISTICS = 1,
    VK_QUERY_TYPE_TIMESTAMP = 2
} VkQueryType;

typedef enum VkFilter {
    VK_FILTER_NEAREST = 0,
    VK_FILTER_LINEAR = 1
} VkFilter;

typedef enum VkSamplerMipmapMode {
    VK_SAMPLER_MIPMAP_MODE_NEAREST = 0,
    VK_SAMPLER_MIPMAP_MODE_LINEAR = 1
} VkSamplerMipmapMode;

typedef enum VkSamplerAddressMode {
    VK_SAMPLER_ADDRESS_MODE_REPEAT = 0,
    VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT = 1,
    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE = 2,
    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER = 3
} VkSamplerAddressMode;

typedef enum VkBorderColor {
    VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK = 0
} VkBorderColor;

typedef enum VkDescriptorType {
    VK_DESCRIPTOR_TYPE_SAMPLER = 0,
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1,
    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE = 2,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE = 3,
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER = 6
} VkDescriptorType;

typedef enum VkVertexInputRate {
    VK_VERTEX_INPUT_RATE_VERTEX = 0,
    VK_VERTEX_INPUT_RATE_INSTANCE = 1
} VkVertexInputRate;

typedef enum VkPrimitiveTopology {
    VK_PRIMITIVE_TOPOLOGY_POINT_LIST = 0,
    VK_PRIMITIVE_TOPOLOGY_LINE_LIST = 1,
    VK_PRIMITIVE_TOPOLOGY_LINE_STRIP = 2,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP = 4,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN = 5
} VkPrimitiveTopology;

typedef enum VkPolygonMode {
    VK_POLYGON_MODE_FILL = 0,
    VK_POLYGON_MODE_LINE = 1,
    VK_POLYGON_MODE_POINT = 2
} VkPolygonMode;

typedef enum VkFrontFace {
    VK_FRONT_FACE_COUNTER_CLOCKWISE = 0,
    VK_FRONT_FACE_CLOCKWISE = 1
} VkFrontFace;

typedef enum VkCompareOp {
    VK_COMPARE_OP_NEVER = 0,
    VK_COMPARE_OP_LESS = 1,
    VK_COMPARE_OP_EQUAL = 2,
    VK_COMPARE_OP_LESS_OR_EQUAL = 3,
    VK_COMPARE_OP_GREATER = 4,
    VK_COMPARE_OP_NOT_EQUAL = 5,
    VK_COMPARE_OP_GREATER_OR_EQUAL = 6,
    VK_COMPARE_OP_ALWAYS = 7
} VkCompareOp;

typedef enum VkStencilOp {
    VK_STENCIL_OP_KEEP = 0
} VkStencilOp;

typedef enum VkLogicOp {
    VK_LOGIC_OP_CLEAR = 0
} VkLogicOp;

typedef enum VkBlendFactor {
    VK_BLEND_FACTOR_ZERO = 0,
    VK_BLEND_FACTOR_ONE = 1,
    VK_BLEND_FACTOR_SRC_COLOR = 2,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR = 3,
    VK_BLEND_FACTOR_DST_COLOR = 4,
    VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR = 5,
    VK_BLEND_FACTOR_SRC_ALPHA = 6,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA = 7,
    VK_BLEND_FACTOR_DST_ALPHA = 8,
    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA = 9,
    VK_BLEND_FACTOR_CONSTANT_COLOR = 10,
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR = 11,
    VK_BLEND_FACTOR_CONSTANT_ALPHA = 12,
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA = 13,
    VK_BLEND_FACTOR_SRC_ALPHA_SATURATE = 14
} VkBlendFactor;

typedef enum VkBlendOp {
    VK_BLEND_OP_ADD = 0,
    VK_BLEND_OP_SUBTRACT = 1,
    VK_BLEND_OP_REVERSE_SUBTRACT = 2,
    VK_BLEND_OP_MIN = 3,
    VK_BLEND_OP_MAX = 4
} VkBlendOp;

typedef enum VkDynamicState {
    VK_DYNAMIC_STATE_VIEWPORT = 0,
    VK_DYNAMIC_STATE_SCISSOR = 1,
    VK_DYNAMIC_STATE_LINE_WIDTH = 2,
    VK_DYNAMIC_STATE_DEPTH_BIAS = 3,
    VK_DYNAMIC_STATE_BLEND_CONSTANTS = 4
} VkDynamicState;

/* Bitmasks */
typedef VkFlags VkQueueFlags;
#define VK_QUEUE_GRAPHICS_BIT 0x00000001U
#define VK_QUEUE_COMPUTE_BIT  0x00000002U
#define VK_QUEUE_TRANSFER_BIT 0x00000004U

typedef VkFlags VkMemoryPropertyFlags;
#define VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT  0x00000001U
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT  0x00000002U
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 0x00000004U
#define VK_MEMORY_PROPERTY_HOST_CACHED_BIT   0x00000008U

typedef VkFlags VkMemoryHeapFlags;

typedef VkFlags VkBufferUsageFlags;
#define VK_BUFFER_USAGE_TRANSFER_SRC_BIT  0x00000001U
#define VK_BUFFER_USAGE_TRANSFER_DST_BIT  0x00000002U
#define VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT 0x00000010U
#define VK_BUFFER_USAGE_INDEX_BUFFER_BIT  0x00000040U
#define VK_BUFFER_USAGE_VERTEX_BUFFER_BIT 0x00000080U

typedef VkFlags VkImageUsageFlags;
#define VK_IMAGE_USAGE_TRANSFER_SRC_BIT             0x00000001U
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT             0x00000002U
#define VK_IMAGE_USAGE_SAMPLED_BIT                  0x00000004U
#define VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT         0x00000010U
#define VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT 0x00000020U

typedef VkFlags VkImageAspectFlags;
#define VK_IMAGE_ASPECT_COLOR_BIT   0x00000001U
#define VK_IMAGE_ASPECT_DEPTH_BIT   0x00000002U
#define VK_IMAGE_ASPECT_STENCIL_BIT 0x00000004U

typedef VkFlags VkSampleCountFlagBits;
typedef VkFlags VkSampleCountFlags;
#define VK_SAMPLE_COUNT_1_BIT 0x00000001U

typedef VkFlags VkCommandPoolCreateFlags;
#define VK_COMMAND_POOL_CREATE_TRANSIENT_BIT            0x00000001U
#define VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT 0x00000002U

typedef VkFlags VkCommandBufferResetFlags;
typedef VkFlags VkCommandBufferUsageFlags;
#define VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT 0x00000001U

typedef VkFlags VkQueryControlFlags;
#define VK_QUERY_CONTROL_PRECISE_BIT 0x00000001U

typedef VkFlags VkQueryResultFlags;
#define VK_QUERY_RESULT_64_BIT   0x00000001U
#define VK_QUERY_RESULT_WAIT_BIT 0x00000002U

typedef VkFlags VkShaderStageFlagBits;
typedef VkFlags VkShaderStageFlags;
#define VK_SHADER_STAGE_VERTEX_BIT   0x00000001U
#define VK_SHADER_STAGE_FRAGMENT_BIT 0x00000010U

typedef VkFlags VkCullModeFlags;
#define VK_CULL_MODE_NONE 0U

typedef VkFlags VkColorComponentFlagBits;
typedef VkFlags VkColorComponentFlags;
#define VK_COLOR_COMPONENT_R_BIT 0x00000001U
#define VK_COLOR_COMPONENT_G_BIT 0x00000002U
#define VK_COLOR_COMPONENT_B_BIT 0x00000004U
#define VK_COLOR_COMPONENT_A_BIT 0x00000008U

typedef VkFlags VkPipelineStageFlagBits;
typedef VkFlags VkPipelineStageFlags;
#define VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT             0x00000001U
#define VK_PIPELINE_STAGE_VERTEX_INPUT_BIT            0x00000004U
#define VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT         0x00000080U
#define VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT    0x00000100U
#define VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT     0x00000200U
#define VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT 0x00000400U
#define VK_PIPELINE_STAGE_TRANSFER_BIT                0x00001000U
#define VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT          0x00002000U
#define VK_PIPELINE_STAGE_HOST_BIT                    0x00004000U

typedef VkFlags VkAccessFlagBits;
typedef VkFlags VkAccessFlags;
#define VK_ACCESS_SHADER_READ_BIT                   0x00000020U
#define VK_ACCESS_COLOR_ATTACHMENT_READ_BIT         0x00000080U
#define VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT        0x00000100U
#define VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT  0x00000200U
#define VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT 0x00000400U
#define VK_ACCESS_TRANSFER_READ_BIT                 0x00000800U
#define VK_ACCESS_TRANSFER_WRITE_BIT                0x00001000U
#define VK_ACCESS_HOST_READ_BIT                     0x00002000U
#define VK_ACCESS_HOST_WRITE_BIT                    0x00004000U

typedef VkFlags VkDependencyFlags;

/* Structs */
typedef struct VkAllocationCallbacks VkAllocationCallbacks;
typedef struct VkSpecializationInfo VkSpecializationInfo;

typedef struct VkOffset2D {
    int32_t x, y;
} VkOffset2D;

typedef struct VkOffset3D {
    int32_t x, y, z;
} VkOffset3D;

typedef struct VkExtent2D {
    uint32_t width, height;
} VkExtent2D;

typedef struct VkExtent3D {
    uint32_t width, height, depth;
} VkExtent3D;

typedef struct VkRect2D {
    VkOffset2D offset;
    VkExtent2D extent;
} VkRect2D;

typedef struct VkViewport {
    float x, y, width, height, minDepth, maxDepth;
} VkViewport;

typedef struct VkApplicationInfo {
    VkStructureType sType;
    const void*     pNext;
    const char*     pApplicationName;
    uint32_t        applicationVersion;
    const char*     pEngineName;
    uint32_t        engineVersion;
    uint32_t        apiVersion;
} VkApplicationInfo;

typedef struct VkInstanceCreateInfo {
    VkStructureType          sType;
    const void*              pNext;
    VkFlags                  flags;
    const VkApplicationInfo* pApplicationInfo;
    uint32_t                 enabledLayerCount;
    const char* const*       ppEnabledLayerNames;
    uint32_t                 enabledExtensionCount;
    const char* const*       ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct VkPhysicalDeviceProperties {
    uint32_t             apiVersion;
    uint32_t             driverVersion;
    uint32_t             vendorID;
    uint32_t             deviceID;
    VkPhysicalDeviceType deviceType;
    char                 deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint8_t              pipelineCacheUUID[VK_UUID_SIZE];
    uint64_t             _limits_and_sparse[72]; /* 576 bytes (>= 528 needed for limits+sparse) */
} VkPhysicalDeviceProperties;

typedef struct VkPhysicalDeviceFeatures {
    VkBool32 robustBufferAccess;
    VkBool32 fullDrawIndexUint32;
    VkBool32 imageCubeArray;
    VkBool32 independentBlend;
    VkBool32 geometryShader;
    VkBool32 tessellationShader;
    VkBool32 sampleRateShading;
    VkBool32 dualSrcBlend;
    VkBool32 logicOp;
    VkBool32 multiDrawIndirect;
    VkBool32 drawIndirectFirstInstance;
    VkBool32 depthClamp;
    VkBool32 depthBiasClamp;
    VkBool32 fillModeNonSolid;
    VkBool32 depthBounds;
    VkBool32 wideLines;
    VkBool32 largePoints;
    VkBool32 alphaToOne;
    VkBool32 multiViewport;
    VkBool32 samplerAnisotropy;
    VkBool32 textureCompressionETC2;
    VkBool32 textureCompressionASTC_LDR;
    VkBool32 textureCompressionBC;
    VkBool32 occlusionQueryPrecise;
    VkBool32 pipelineStatisticsQuery;
    VkBool32 vertexPipelineStoresAndAtomics;
    VkBool32 fragmentStoresAndAtomics;
    VkBool32 shaderTessellationAndGeometryPointSize;
    VkBool32 shaderImageGatherExtended;
    VkBool32 shaderStorageImageExtendedFormats;
    VkBool32 shaderStorageImageMultisample;
    VkBool32 shaderStorageImageReadWithoutFormat;
    VkBool32 shaderStorageImageWriteWithoutFormat;
    VkBool32 shaderUniformBufferArrayDynamicIndexing;
    VkBool32 shaderSampledImageArrayDynamicIndexing;
    VkBool32 shaderStorageBufferArrayDynamicIndexing;
    VkBool32 shaderStorageImageArrayDynamicIndexing;
    VkBool32 shaderClipDistance;
    VkBool32 shaderCullDistance;
    VkBool32 shaderFloat64;
    VkBool32 shaderInt64;
    VkBool32 shaderInt16;
    VkBool32 shaderResourceResidency;
    VkBool32 shaderResourceMinLod;
    VkBool32 sparseBinding;
    VkBool32 sparseResidencyBuffer;
    VkBool32 sparseResidencyImage2D;
    VkBool32 sparseResidencyImage3D;
    VkBool32 sparseResidency2Samples;
    VkBool32 sparseResidency4Samples;
    VkBool32 sparseResidency8Samples;
    VkBool32 sparseResidency16Samples;
    VkBool32 sparseResidencyAliased;
    VkBool32 variableMultisampleRate;
    VkBool32 inheritedQueries;
} VkPhysicalDeviceFeatures;

typedef struct VkMemoryType {
    VkMemoryPropertyFlags propertyFlags;
    uint32_t              heapIndex;
} VkMemoryType;

typedef struct VkMemoryHeap {
    VkDeviceSize      size;
    VkMemoryHeapFlags flags;
} VkMemoryHeap;

typedef struct VkPhysicalDeviceMemoryProperties {
    uint32_t     memoryTypeCount;
    VkMemoryType memoryTypes[VK_MAX_MEMORY_TYPES];
    uint32_t     memoryHeapCount;
    VkMemoryHeap memoryHeaps[VK_MAX_MEMORY_HEAPS];
} VkPhysicalDeviceMemoryProperties;

typedef struct VkQueueFamilyProperties {
    VkQueueFlags queueFlags;
    uint32_t     queueCount;
    uint32_t     timestampValidBits;
    VkExtent3D   minImageTransferGranularity;
} VkQueueFamilyProperties;

typedef struct VkDeviceQueueCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    uint32_t        queueFamilyIndex;
    uint32_t        queueCount;
    const float*    pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct VkDeviceCreateInfo {
    VkStructureType                sType;
    const void*                    pNext;
    VkFlags                        flags;
    uint32_t                       queueCreateInfoCount;
    const VkDeviceQueueCreateInfo* pQueueCreateInfos;
    uint32_t                       enabledLayerCount;
    const char* const*             ppEnabledLayerNames;
    uint32_t                       enabledExtensionCount;
    const char* const*             ppEnabledExtensionNames;
    const VkPhysicalDeviceFeatures* pEnabledFeatures;
} VkDeviceCreateInfo;

typedef struct VkCommandPoolCreateInfo {
    VkStructureType          sType;
    const void*              pNext;
    VkCommandPoolCreateFlags flags;
    uint32_t                 queueFamilyIndex;
} VkCommandPoolCreateInfo;

typedef struct VkCommandBufferAllocateInfo {
    VkStructureType      sType;
    const void*          pNext;
    VkCommandPool        commandPool;
    VkCommandBufferLevel level;
    uint32_t             commandBufferCount;
} VkCommandBufferAllocateInfo;

typedef struct VkFenceCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
} VkFenceCreateInfo;

typedef struct VkQueryPoolCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    VkQueryType     queryType;
    uint32_t        queryCount;
    VkFlags         pipelineStatistics;
} VkQueryPoolCreateInfo;

typedef struct VkMemoryRequirements {
    VkDeviceSize size;
    VkDeviceSize alignment;
    uint32_t     memoryTypeBits;
} VkMemoryRequirements;

typedef struct VkMemoryAllocateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkDeviceSize    allocationSize;
    uint32_t        memoryTypeIndex;
} VkMemoryAllocateInfo;

typedef struct VkBufferCreateInfo {
    VkStructureType    sType;
    const void*        pNext;
    VkFlags            flags;
    VkDeviceSize       size;
    VkBufferUsageFlags usage;
    VkSharingMode      sharingMode;
    uint32_t           queueFamilyIndexCount;
    const uint32_t*    pQueueFamilyIndices;
} VkBufferCreateInfo;

typedef struct VkImageCreateInfo {
    VkStructureType       sType;
    const void*           pNext;
    VkFlags               flags;
    VkImageType           imageType;
    VkFormat              format;
    VkExtent3D            extent;
    uint32_t              mipLevels;
    uint32_t              arrayLayers;
    VkSampleCountFlagBits samples;
    VkImageTiling         tiling;
    VkImageUsageFlags     usage;
    VkSharingMode         sharingMode;
    uint32_t              queueFamilyIndexCount;
    const uint32_t*       pQueueFamilyIndices;
    VkImageLayout         initialLayout;
} VkImageCreateInfo;

typedef struct VkComponentMapping {
    VkComponentSwizzle r, g, b, a;
} VkComponentMapping;

typedef struct VkImageSubresourceRange {
    VkImageAspectFlags aspectMask;
    uint32_t           baseMipLevel;
    uint32_t           levelCount;
    uint32_t           baseArrayLayer;
    uint32_t           layerCount;
} VkImageSubresourceRange;

typedef struct VkImageViewCreateInfo {
    VkStructureType         sType;
    const void*             pNext;
    VkFlags                 flags;
    VkImage                 image;
    VkImageViewType         viewType;
    VkFormat                format;
    VkComponentMapping      components;
    VkImageSubresourceRange subresourceRange;
} VkImageViewCreateInfo;

typedef struct VkAttachmentDescription {
    VkFlags               flags;
    VkFormat              format;
    VkSampleCountFlagBits samples;
    VkAttachmentLoadOp    loadOp;
    VkAttachmentStoreOp   storeOp;
    VkAttachmentLoadOp    stencilLoadOp;
    VkAttachmentStoreOp   stencilStoreOp;
    VkImageLayout         initialLayout;
    VkImageLayout         finalLayout;
} VkAttachmentDescription;

typedef struct VkAttachmentReference {
    uint32_t      attachment;
    VkImageLayout layout;
} VkAttachmentReference;

typedef struct VkSubpassDescription {
    VkFlags                      flags;
    VkPipelineBindPoint          pipelineBindPoint;
    uint32_t                     inputAttachmentCount;
    const VkAttachmentReference* pInputAttachments;
    uint32_t                     colorAttachmentCount;
    const VkAttachmentReference* pColorAttachments;
    const VkAttachmentReference* pResolveAttachments;
    const VkAttachmentReference* pDepthStencilAttachment;
    uint32_t                     preserveAttachmentCount;
    const uint32_t*              pPreserveAttachments;
} VkSubpassDescription;

typedef struct VkSubpassDependency {
    uint32_t             srcSubpass;
    uint32_t             dstSubpass;
    VkPipelineStageFlags srcStageMask;
    VkPipelineStageFlags dstStageMask;
    VkAccessFlags        srcAccessMask;
    VkAccessFlags        dstAccessMask;
    VkDependencyFlags    dependencyFlags;
} VkSubpassDependency;

typedef struct VkRenderPassCreateInfo {
    VkStructureType                sType;
    const void*                    pNext;
    VkFlags                        flags;
    uint32_t                       attachmentCount;
    const VkAttachmentDescription* pAttachments;
    uint32_t                       subpassCount;
    const VkSubpassDescription*    pSubpasses;
    uint32_t                       dependencyCount;
    const VkSubpassDependency*     pDependencies;
} VkRenderPassCreateInfo;

typedef struct VkFramebufferCreateInfo {
    VkStructureType    sType;
    const void*        pNext;
    VkFlags            flags;
    VkRenderPass       renderPass;
    uint32_t           attachmentCount;
    const VkImageView* pAttachments;
    uint32_t           width;
    uint32_t           height;
    uint32_t           layers;
} VkFramebufferCreateInfo;

typedef struct VkSamplerCreateInfo {
    VkStructureType      sType;
    const void*          pNext;
    VkFlags              flags;
    VkFilter             magFilter;
    VkFilter             minFilter;
    VkSamplerMipmapMode  mipmapMode;
    VkSamplerAddressMode addressModeU;
    VkSamplerAddressMode addressModeV;
    VkSamplerAddressMode addressModeW;
    float                mipLodBias;
    VkBool32             anisotropyEnable;
    float                maxAnisotropy;
    VkBool32             compareEnable;
    VkCompareOp          compareOp;
    float                minLod;
    float                maxLod;
    VkBorderColor        borderColor;
    VkBool32             unnormalizedCoordinates;
} VkSamplerCreateInfo;

typedef struct VkDescriptorSetLayoutBinding {
    uint32_t           binding;
    VkDescriptorType   descriptorType;
    uint32_t           descriptorCount;
    VkShaderStageFlags stageFlags;
    const VkSampler*   pImmutableSamplers;
} VkDescriptorSetLayoutBinding;

typedef struct VkDescriptorSetLayoutCreateInfo {
    VkStructureType                     sType;
    const void*                         pNext;
    VkFlags                             flags;
    uint32_t                            bindingCount;
    const VkDescriptorSetLayoutBinding* pBindings;
} VkDescriptorSetLayoutCreateInfo;

typedef struct VkDescriptorPoolSize {
    VkDescriptorType type;
    uint32_t         descriptorCount;
} VkDescriptorPoolSize;

typedef struct VkDescriptorPoolCreateInfo {
    VkStructureType             sType;
    const void*                 pNext;
    VkFlags                     flags;
    uint32_t                    maxSets;
    uint32_t                    poolSizeCount;
    const VkDescriptorPoolSize* pPoolSizes;
} VkDescriptorPoolCreateInfo;

typedef struct VkDescriptorSetAllocateInfo {
    VkStructureType              sType;
    const void*                  pNext;
    VkDescriptorPool             descriptorPool;
    uint32_t                     descriptorSetCount;
    const VkDescriptorSetLayout* pSetLayouts;
} VkDescriptorSetAllocateInfo;

typedef struct VkDescriptorImageInfo {
    VkSampler     sampler;
    VkImageView   imageView;
    VkImageLayout imageLayout;
} VkDescriptorImageInfo;

typedef struct VkDescriptorBufferInfo {
    VkBuffer     buffer;
    VkDeviceSize offset;
    VkDeviceSize range;
} VkDescriptorBufferInfo;

typedef struct VkWriteDescriptorSet {
    VkStructureType               sType;
    const void*                   pNext;
    VkDescriptorSet               dstSet;
    uint32_t                      dstBinding;
    uint32_t                      dstArrayElement;
    uint32_t                      descriptorCount;
    VkDescriptorType              descriptorType;
    const VkDescriptorImageInfo*  pImageInfo;
    const VkDescriptorBufferInfo* pBufferInfo;
    const VkBufferView*           pTexelBufferView;
} VkWriteDescriptorSet;

typedef struct VkCopyDescriptorSet VkCopyDescriptorSet;

typedef struct VkPushConstantRange {
    VkShaderStageFlags stageFlags;
    uint32_t           offset;
    uint32_t           size;
} VkPushConstantRange;

typedef struct VkPipelineLayoutCreateInfo {
    VkStructureType              sType;
    const void*                  pNext;
    VkFlags                      flags;
    uint32_t                     setLayoutCount;
    const VkDescriptorSetLayout* pSetLayouts;
    uint32_t                     pushConstantRangeCount;
    const VkPushConstantRange*   pPushConstantRanges;
} VkPipelineLayoutCreateInfo;

typedef struct VkShaderModuleCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    size_t          codeSize;
    const uint32_t* pCode;
} VkShaderModuleCreateInfo;

typedef struct VkPipelineShaderStageCreateInfo {
    VkStructureType             sType;
    const void*                 pNext;
    VkFlags                     flags;
    VkShaderStageFlagBits       stage;
    VkShaderModule              module;
    const char*                 pName;
    const VkSpecializationInfo* pSpecializationInfo;
} VkPipelineShaderStageCreateInfo;

typedef struct VkVertexInputBindingDescription {
    uint32_t          binding;
    uint32_t          stride;
    VkVertexInputRate inputRate;
} VkVertexInputBindingDescription;

typedef struct VkVertexInputAttributeDescription {
    uint32_t location;
    uint32_t binding;
    VkFormat format;
    uint32_t offset;
} VkVertexInputAttributeDescription;

typedef struct VkPipelineVertexInputStateCreateInfo {
    VkStructureType                            sType;
    const void*                                pNext;
    VkFlags                                    flags;
    uint32_t                                   vertexBindingDescriptionCount;
    const VkVertexInputBindingDescription*     pVertexBindingDescriptions;
    uint32_t                                   vertexAttributeDescriptionCount;
    const VkVertexInputAttributeDescription*   pVertexAttributeDescriptions;
} VkPipelineVertexInputStateCreateInfo;

typedef struct VkPipelineInputAssemblyStateCreateInfo {
    VkStructureType     sType;
    const void*         pNext;
    VkFlags             flags;
    VkPrimitiveTopology topology;
    VkBool32            primitiveRestartEnable;
} VkPipelineInputAssemblyStateCreateInfo;

typedef struct VkPipelineTessellationStateCreateInfo VkPipelineTessellationStateCreateInfo;

typedef struct VkPipelineViewportStateCreateInfo {
    VkStructureType   sType;
    const void*       pNext;
    VkFlags           flags;
    uint32_t          viewportCount;
    const VkViewport* pViewports;
    uint32_t          scissorCount;
    const VkRect2D*   pScissors;
} VkPipelineViewportStateCreateInfo;

typedef struct VkPipelineRasterizationStateCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    VkBool32        depthClampEnable;
    VkBool32        rasterizerDiscardEnable;
    VkPolygonMode   polygonMode;
    VkCullModeFlags cullMode;
    VkFrontFace     frontFace;
    VkBool32        depthBiasEnable;
    float           depthBiasConstantFactor;
    float           depthBiasClamp;
    float           depthBiasSlopeFactor;
    float           lineWidth;
} VkPipelineRasterizationStateCreateInfo;

typedef struct VkPipelineMultisampleStateCreateInfo {
    VkStructureType       sType;
    const void*           pNext;
    VkFlags               flags;
    VkSampleCountFlagBits rasterizationSamples;
    VkBool32              sampleShadingEnable;
    float                 minSampleShading;
    const VkSampleMask*   pSampleMask;
    VkBool32              alphaToCoverageEnable;
    VkBool32              alphaToOneEnable;
} VkPipelineMultisampleStateCreateInfo;

typedef struct VkStencilOpState {
    VkStencilOp failOp;
    VkStencilOp passOp;
    VkStencilOp depthFailOp;
    VkCompareOp compareOp;
    uint32_t    compareMask;
    uint32_t    writeMask;
    uint32_t    reference;
} VkStencilOpState;

typedef struct VkPipelineDepthStencilStateCreateInfo {
    VkStructureType  sType;
    const void*      pNext;
    VkFlags          flags;
    VkBool32         depthTestEnable;
    VkBool32         depthWriteEnable;
    VkCompareOp      depthCompareOp;
    VkBool32         depthBoundsTestEnable;
    VkBool32         stencilTestEnable;
    VkStencilOpState front;
    VkStencilOpState back;
    float            minDepthBounds;
    float            maxDepthBounds;
} VkPipelineDepthStencilStateCreateInfo;

typedef struct VkPipelineColorBlendAttachmentState {
    VkBool32              blendEnable;
    VkBlendFactor         srcColorBlendFactor;
    VkBlendFactor         dstColorBlendFactor;
    VkBlendOp             colorBlendOp;
    VkBlendFactor         srcAlphaBlendFactor;
    VkBlendFactor         dstAlphaBlendFactor;
    VkBlendOp             alphaBlendOp;
    VkColorComponentFlags colorWriteMask;
} VkPipelineColorBlendAttachmentState;

typedef struct VkPipelineColorBlendStateCreateInfo {
    VkStructureType                            sType;
    const void*                                pNext;
    VkFlags                                    flags;
    VkBool32                                   logicOpEnable;
    VkLogicOp                                  logicOp;
    uint32_t                                   attachmentCount;
    const VkPipelineColorBlendAttachmentState* pAttachments;
    float                                      blendConstants[4];
} VkPipelineColorBlendStateCreateInfo;

typedef struct VkPipelineDynamicStateCreateInfo {
    VkStructureType      sType;
    const void*          pNext;
    VkFlags              flags;
    uint32_t             dynamicStateCount;
    const VkDynamicState* pDynamicStates;
} VkPipelineDynamicStateCreateInfo;

typedef struct VkGraphicsPipelineCreateInfo {
    VkStructureType                               sType;
    const void*                                   pNext;
    VkFlags                                       flags;
    uint32_t                                      stageCount;
    const VkPipelineShaderStageCreateInfo*        pStages;
    const VkPipelineVertexInputStateCreateInfo*   pVertexInputState;
    const VkPipelineInputAssemblyStateCreateInfo* pInputAssemblyState;
    const VkPipelineTessellationStateCreateInfo*  pTessellationState;
    const VkPipelineViewportStateCreateInfo*      pViewportState;
    const VkPipelineRasterizationStateCreateInfo* pRasterizationState;
    const VkPipelineMultisampleStateCreateInfo*   pMultisampleState;
    const VkPipelineDepthStencilStateCreateInfo*  pDepthStencilState;
    const VkPipelineColorBlendStateCreateInfo*    pColorBlendState;
    const VkPipelineDynamicStateCreateInfo*       pDynamicState;
    VkPipelineLayout                              layout;
    VkRenderPass                                  renderPass;
    uint32_t                                      subpass;
    VkPipeline                                    basePipelineHandle;
    int32_t                                       basePipelineIndex;
} VkGraphicsPipelineCreateInfo;

typedef struct VkCommandBufferBeginInfo {
    VkStructureType            sType;
    const void*                pNext;
    VkCommandBufferUsageFlags  flags;
    const void*                pInheritanceInfo;
} VkCommandBufferBeginInfo;

typedef struct VkMemoryBarrier {
    VkStructureType sType;
    const void*     pNext;
    VkAccessFlags   srcAccessMask;
    VkAccessFlags   dstAccessMask;
} VkMemoryBarrier;

typedef struct VkBufferMemoryBarrier VkBufferMemoryBarrier;

typedef struct VkImageMemoryBarrier {
    VkStructureType         sType;
    const void*             pNext;
    VkAccessFlags           srcAccessMask;
    VkAccessFlags           dstAccessMask;
    VkImageLayout           oldLayout;
    VkImageLayout           newLayout;
    uint32_t                srcQueueFamilyIndex;
    uint32_t                dstQueueFamilyIndex;
    VkImage                 image;
    VkImageSubresourceRange subresourceRange;
} VkImageMemoryBarrier;

typedef struct VkImageSubresourceLayers {
    VkImageAspectFlags aspectMask;
    uint32_t           mipLevel;
    uint32_t           baseArrayLayer;
    uint32_t           layerCount;
} VkImageSubresourceLayers;

typedef struct VkBufferImageCopy {
    VkDeviceSize             bufferOffset;
    uint32_t                 bufferRowLength;
    uint32_t                 bufferImageHeight;
    VkImageSubresourceLayers imageSubresource;
    VkOffset3D               imageOffset;
    VkExtent3D               imageExtent;
} VkBufferImageCopy;

typedef union VkClearColorValue {
    float    float32[4];
    int32_t  int32[4];
    uint32_t uint32[4];
} VkClearColorValue;

typedef struct VkClearDepthStencilValue {
    float    depth;
    uint32_t stencil;
} VkClearDepthStencilValue;

typedef union VkClearValue {
    VkClearColorValue        color;
    VkClearDepthStencilValue depthStencil;
} VkClearValue;

typedef struct VkRenderPassBeginInfo {
    VkStructureType     sType;
    const void*         pNext;
    VkRenderPass        renderPass;
    VkFramebuffer       framebuffer;
    VkRect2D            renderArea;
    uint32_t            clearValueCount;
    const VkClearValue* pClearValues;
} VkRenderPassBeginInfo;

typedef struct VkSubmitInfo {
    VkStructureType              sType;
    const void*                  pNext;
    uint32_t                     waitSemaphoreCount;
    const VkSemaphore*           pWaitSemaphores;
    const VkPipelineStageFlags*  pWaitDstStageMask;
    uint32_t                     commandBufferCount;
    const VkCommandBuffer*       pCommandBuffers;
    uint32_t                     signalSemaphoreCount;
    const VkSemaphore*           pSignalSemaphores;
} VkSubmitInfo;

/* Function declarations */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance, uint32_t*, VkPhysicalDevice*);
VKAPI_ATTR void     VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties*);
VKAPI_ATTR void     VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*);
VKAPI_ATTR void     VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures*);
VKAPI_ATTR void     VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
VKAPI_ATTR void     VKAPI_CALL vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool*);
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice, const VkQueryPoolCreateInfo*, const VkAllocationCallbacks*, VkQueryPool*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage*);
VKAPI_ATTR void     VKAPI_CALL vkGetImageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements*);
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*);
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView*);
VKAPI_ATTR void     VKAPI_CALL vkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks*);
VKAPI_ATTR void     VKAPI_CALL vkDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks*);
VKAPI_ATTR void     VKAPI_CALL vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice, const VkRenderPassCreateInfo*, const VkAllocationCallbacks*, VkRenderPass*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice, const VkFramebufferCreateInfo*, const VkAllocationCallbacks*, VkFramebuffer*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer*);
VKAPI_ATTR void     VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements*);
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice, const VkSamplerCreateInfo*, const VkAllocationCallbacks*, VkSampler*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool*);
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*);
VKAPI_ATTR void     VKAPI_CALL vkUpdateDescriptorSets(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice);
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer, VkCommandBufferResetFlags);
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*);
VKAPI_ATTR void     VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*);
VKAPI_ATTR void     VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t, const VkBufferImageCopy*);
VKAPI_ATTR void     VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t);
VKAPI_ATTR void     VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents);
VKAPI_ATTR void     VKAPI_CALL vkCmdSetViewport(VkCommandBuffer, uint32_t, uint32_t, const VkViewport*);
VKAPI_ATTR void     VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer*, const VkDeviceSize*);
VKAPI_ATTR void     VKAPI_CALL vkCmdBeginQuery(VkCommandBuffer, VkQueryPool, uint32_t, VkQueryControlFlags);
VKAPI_ATTR void     VKAPI_CALL vkCmdEndQuery(VkCommandBuffer, VkQueryPool, uint32_t);
VKAPI_ATTR void     VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
VKAPI_ATTR void     VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*);
VKAPI_ATTR void     VKAPI_CALL vkCmdSetScissor(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D*);
VKAPI_ATTR void     VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer, const float[4]);
VKAPI_ATTR void     VKAPI_CALL vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void*);
VKAPI_ATTR void     VKAPI_CALL vkCmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
VKAPI_ATTR void     VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer);
VKAPI_ATTR void     VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy*);
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer);
VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice, uint32_t, const VkFence*);
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t);
VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice, VkQueryPool, uint32_t, uint32_t, size_t, void*, VkDeviceSize, VkQueryResultFlags);

#endif /* PSPRECOMP_VK_MINI_H */
