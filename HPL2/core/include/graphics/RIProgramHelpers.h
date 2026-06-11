#ifndef HPL_RI_PROGRAM_HELPERS_H
#define HPL_RI_PROGRAM_HELPERS_H

#include "graphics/RIProgram.h"
#include "graphics/RITypes.h"
#include "resources/Resources.h"

#include <span>

namespace hpl {

// Load a single-stage Slang compute program. `name` is the .spv filename
// resolved through the resource searcher; `entryPoint` is the SPIR-V
// OpEntryPoint function name (slangc is invoked with
// -fvk-use-entrypoint-name so Slang function names survive into the SPV).
void LoadSlangCompute(RIDevice *device, RIProgram &prog,
                      cResources *resources, const char *name,
                      const char *entryPoint,
                      std::span<const VkDescriptorSetLayout> externalLayouts = {});

// Load a Slang vert+frag program. When `vertName == fragName` a single
// .spv blob is reused for both stages (the same idiom as the m_gbuffer
// load). Pass distinct names when the stages are compiled to separate
// .spv files (the common case for post-effects sharing one fullscreen
// vert with many frags).
void LoadSlangGraphics(RIDevice *device, RIProgram &prog,
                       cResources *resources, const char *vertName,
                       const char *fragName,
                       const char *vertEntryPoint = "vsMain",
                       const char *fragEntryPoint = "psMain",
                       std::span<const VkDescriptorSetLayout> externalLayouts = {});

} // namespace hpl

#endif
