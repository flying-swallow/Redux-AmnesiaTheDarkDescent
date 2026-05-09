#ifndef HPL_GRAPHICS_CONFIG_H
#define HPL_GRAPHICS_CONFIG_H

// Engine-level graphics tuning constants. These describe how the HPL engine
// paces frames, sub-commands, and frame-scoped allocators on top of the RI
// hardware-abstraction layer. They are not part of the RI layer itself and
// are passed through at swapchain / command-ring / segment-allocator init.

#define RI_NUMBER_FRAMES_FLIGHT 2
#define RI_NUMBER_SUB_COMMANDS 16
#define RI_NUMBER_FRAME_SEGMENTS 6

#endif
