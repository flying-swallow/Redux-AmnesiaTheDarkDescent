#define DEVICE_SUPPORT_VULKAN

// Compile-time maximums for templated/array-sized backing storage.
#define RI_MAX_SWAPCHAIN_IMAGES 8

#ifdef DEVICE_SUPPORT_VULKAN
#define VK_NO_PROPERTIES
#define DEVICE_IMPL_VULKAN 1
#else
#define DEVICE_IMPL_VULKAN 0
#endif

#ifdef DEVICE_SUPPORT_MTL
#define DEVICE_IMPL_MTL 1
#else
#define DEVICE_IMPL_MTL 0
#endif

#ifdef DEVICE_SUPPORT_D3D11
#define DEVICE_IMPL_D3D11 1
#else
#define DEVICE_IMPL_D3D11 0
#endif

#ifdef DEVICE_SUPPORT_D3D12
#define DEVICE_IMPL_D3D12 1
#else
#define DEVICE_IMPL_D3D12 0
#endif

// True when more than one backend is compiled in. Used by
// RIRenderer::is_target_selected to decide whether to runtime-dispatch; in
// single-backend builds it is 0, so that check reduces to an assert.
#define DEVICE_MULTI_BACKEND ( ( DEVICE_IMPL_D3D12 + DEVICE_IMPL_D3D11 + DEVICE_IMPL_MTL + DEVICE_IMPL_VULKAN ) > 1 )



