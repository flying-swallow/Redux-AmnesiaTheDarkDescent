#ifndef RI_SWAPCHAIN_H
#define RI_SWAPCHAIN_H

// Swapchain types + acquire/present API, grouped by use case (mirrors
// ref_nri/ri_swapchain.h). The inline dispose() dereferences RIDevice and calls
// RIGetVkInstance / RIIsTargetSelected, so this header includes the full
// RIDevice.h — RIDevice does not reference RISwapchain, so the dependency is
// one-way (no cycle).
#include "graphics/RIPreamble.h"
#include "graphics/RITexture.h"      // RITexture textures[]
#include "graphics/RITextureView.h" // RITextureView views[]
#include "graphics/RICommand.h"     // RIQueue (presentQueue)
#include "graphics/RIDevice.h"      // RIDevice (inline dispose) + backend helpers
#include <variant>                  // RISwapchainDesc::source

enum RISwapchainFormat_e {
  RI_SWAPCHAIN_BT709_G10_16BIT,
  RI_SWAPCHAIN_BT709_G22_8BIT,
  RI_SWAPCHAIN_BT709_G22_10BIT,
  RI_SWAPCHAIN_BT2020_G2084_10BIT
};

enum RIWindowType_e {
  RI_WINDOW_UNKNOWN,
  RI_WINDOW_X11,
  RI_WINDOW_WIN32,
  RI_WINDOW_METAL,
  RI_WINDOW_WAYLAND
};

// The GPU swapchain: per-image render targets plus the binary acquire/finish
// semaphores. The struct + dispose() below use RITexture / RIQueue / RIDevice /
// RIGetVkInstance / RIIsTargetSelected from the domain headers included above.
struct RISwapchain {
  RISwapchain() { memset(this, 0, sizeof(*this)); }
  static constexpr uint32_t MAX_IMAGE_COUNT = RI_MAX_SWAPCHAIN_IMAGES;

  // Destroys the per-image views, the per-image semaphores, the swapchain
  // (the swapchain images themselves are owned by the swapchain) and the
  // surface — the swapchain now OWNS its surface. On a recreate the surface is
  // transferred to the new swapchain (see create()), so a retired swapchain has
  // vk.surface == NULL here and only the last live swapchain frees it.
  void dispose(struct RIDevice *device);
  bool isEmpty() const { return imageCount == 0; }
  // A usable swapchain: has images and a non-degenerate extent.
  bool IsValid() const { return imageCount > 0 && width > 0 && height > 0; }

  // Per-image color-attachment view onto swapchain image `index`.
  struct RITextureView *textureView(uint32_t index) { return &views[index]; }

  // Creates a swapchain (+ per-image views). `desc.source` selects the surface:
  // a RIWindowHandle makes a fresh surface the new swapchain adopts; a
  // RISwapchain* reuses that swapchain's surface (ownership transferred to the
  // new one) and hands its VkSwapchainKHR to oldSwapchain. Returns an empty
  // swapchain on failure.
  static RISwapchain create(struct RIDevice *device,
                            const struct RISwapchainDesc &desc);

  struct RIQueue *presentQueue;
  uint16_t imageCount;
  uint16_t width;
  uint16_t height;
  uint32_t format; // RI_Format_e
  struct RITexture textures[RI_MAX_SWAPCHAIN_IMAGES];
  // Per-image color-attachment views (owned by the swapchain). RITextureView is
  // cross-backend so this lives outside the vk union.
  struct RITextureView views[RI_MAX_SWAPCHAIN_IMAGES];
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      uint32_t frameIndex;
      uint32_t textureIndex;
      uint64_t presentID;
      // Owned by the swapchain; transferred to the successor on recreate and
      // freed by dispose() on the last live swapchain (NULL once transferred).
      VkSurfaceKHR surface;
      VkSwapchainKHR swapchain;
      VkImage images[RI_MAX_SWAPCHAIN_IMAGES];
      VkSemaphore imageAcquireSem[RI_MAX_SWAPCHAIN_IMAGES];
      VkSemaphore finishSem[RI_MAX_SWAPCHAIN_IMAGES];
    } vk;
#endif
  };
};

inline void RISwapchain::dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    for (uint32_t p = 0; p < RI_MAX_SWAPCHAIN_IMAGES; p++) {
      views[p].dispose(device);
      if (vk.imageAcquireSem[p])
        vkDestroySemaphore(device->vk.device, vk.imageAcquireSem[p], NULL);
      if (vk.finishSem[p])
        vkDestroySemaphore(device->vk.device, vk.finishSem[p], NULL);
    }
    if (vk.swapchain)
      vkDestroySwapchainKHR(device->vk.device, vk.swapchain, NULL);
    // The surface (needs the instance, not the device) is owned by the
    // swapchain. Retired swapchains have already transferred it (NULL), so only
    // the last live swapchain frees it here — after its VkSwapchainKHR is gone.
    if (vk.surface)
      vkDestroySurfaceKHR(RIGetVkInstance(), vk.surface, NULL);
  }
#endif
}

struct RIWindowHandle {
	uint8_t type; // RIWindowType_e
	union {
		struct {
    	void* hwnd; // HWND
    	//void* surface; //HSURFACE
		} windows;
		struct {
    	void* dpy; // Display*
    	uint64_t window; // Window
		} x11;
		struct {
    	void* display; // wl_display*
    	void* surface; // wl_surface*
		} wayland;
		struct {
    	void* caMetalLayer; // CAMetalLayer*
		} metal;
	};
};

struct RISwapchainDesc {
	uint8_t format; // RISwapchainFormat_e
	uint16_t requestImageCount;
	struct RIQueue* queue;
	uint16_t width, height;
	bool vsync; // true -> FIFO (v-synced); false -> IMMEDIATE/MAILBOX when supported
	// Where the surface comes from:
	//  - RIWindowHandle  : first create — create() makes a fresh surface
	//                      (RICreateWindowSurface) that the new swapchain adopts.
	//  - RISwapchain*    : recreate — reuse that swapchain's surface (ownership is
	//                      transferred to the new swapchain) and hand its
	//                      VkSwapchainKHR to vkCreateSwapchainKHR's oldSwapchain so
	//                      the driver can reuse resources.
	std::variant<RIWindowHandle, struct RISwapchain*> source;
};

// Result of a swapchain acquire/present. OUT_OF_DATE means the swapchain is no
// longer usable (window/surface resized) and must be recreated before the next
// acquire; SUBOPTIMAL means it still presented this frame but should be recreated
// soon (e.g. the surface size drifted).
enum RISwapchainStatus_e {
	RI_SWAPCHAIN_STATUS_OK = 0,
	RI_SWAPCHAIN_STATUS_OUT_OF_DATE,
	RI_SWAPCHAIN_STATUS_SUBOPTIMAL,
};

// Creates a platform VkSurfaceKHR from a window handle. Owned by the caller
// (cGraphics), destroyed with vkDestroySurfaceKHR. Returns VK_NULL_HANDLE on
// failure / unsupported platform.
VkSurfaceKHR RICreateWindowSurface(const struct RIWindowHandle *handle);
// Acquires the next swapchain image. On OK/SUBOPTIMAL the image index is written to
// *outTextureIndex; on OUT_OF_DATE nothing is written and the caller must recreate.
RISwapchainStatus_e RISwapchainAcquireNextTexture(struct RIDevice* dev, RISwapchain* swapchain, uint32_t* outTextureIndex);
RISwapchainStatus_e RISwapchainPresent(struct RIDevice* dev, RISwapchain* swapchain);

#endif

