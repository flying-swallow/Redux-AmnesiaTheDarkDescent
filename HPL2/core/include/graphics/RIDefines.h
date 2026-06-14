// Backend is selected by the build system via one of DEVICE_SUPPORT_{VULKAN,MTL,
// D3D11,D3D12} (see ENABLE_VULKAN/ENABLE_METAL in HPL2/CMakeLists.txt). Default to Vulkan
// when nothing is defined so existing/standalone builds are unaffected.
#if !defined(DEVICE_SUPPORT_VULKAN) && !defined(DEVICE_SUPPORT_MTL) &&          \
    !defined(DEVICE_SUPPORT_D3D11) && !defined(DEVICE_SUPPORT_D3D12)
#define DEVICE_SUPPORT_VULKAN
#endif

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

#define DEVICE_MULTI_BACKEND (DEVICE_IMPL_VULKAN + DEVICE_IMPL_MTL + DEVICE_IMPL_D3D11 + DEVICE_IMPL_D3D12 > 1)



