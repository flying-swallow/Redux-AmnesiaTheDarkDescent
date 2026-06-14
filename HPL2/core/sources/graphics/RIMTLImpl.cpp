// metal-cpp is header-only: exactly ONE translation unit must define the
// *_PRIVATE_IMPLEMENTATION macros before including the umbrella headers, which is
// what actually emits all NS::/CA::/MTL:: selector + private-class symbols. Without
// it every MTL::Private::Selector::s_k* reference is undefined at link. This is that
// TU. Guarded by DEVICE_IMPL_MTL so it compiles to an empty object on non-Metal
// builds (the graphics source GLOB compiles it unconditionally, and the metal-cpp
// include path only exists on APPLE).
#include "graphics/RIDefines.h"

#if (DEVICE_IMPL_MTL)
#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#endif
