#ifndef RI_SWAPCHAIN_H
#define RI_SWAPCHAIN_H

#include "RITypes.h"

struct RIWindowHandle_s {
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

struct RISwapchainDesc_s {
	uint8_t format; // RISwapchainFormat_e
	uint16_t requestImageCount;
	struct RIWindowHandle_s* windowHandle;
	struct RIQueue_s* queue;
	uint16_t width, height;
};

int InitRISwapchain(struct RIDevice_s* dev, struct RISwapchainDesc_s* init, RISwapchain_s<>* swapchain);
uint32_t RISwapchainAcquireNextTexture(struct RIDevice_s* dev, RISwapchain_s<>* swapchain);
void RISwapchainPresent(struct RIDevice_s* dev, RISwapchain_s<>* swapchain);

static inline bool IsRISwapchainValid( RISwapchain_s<> *swapchain )
{
	return swapchain->imageCount > 0 && swapchain->width > 0 && swapchain->height > 0;
}

#endif

