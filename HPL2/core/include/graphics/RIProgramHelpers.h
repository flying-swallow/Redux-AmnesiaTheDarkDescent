#ifndef HPL_RI_PROGRAM_HELPERS_H
#define HPL_RI_PROGRAM_HELPERS_H

#include "graphics/RIProgram.h"
#include "graphics/RITypes.h"
#include "resources/Resources.h"

#include <span>

namespace hpl {

// Load a single-stage Slang compute program. `name` is the .spv filename
// resolved through the resource searcher; `entryPoint` is the SPIR-V
// OpEntryPoint function name (slangc is invoked with -fvk-use-entrypoint-name so
// Slang function names survive into the SPV). `bindings` is the unified,
// combined-stage binding table; the push-constant block (if any) is implicitly
// visible to the compute stage.
void LoadSlangCompute(RIDevice *device, RIProgram &prog,
                      cResources *resources, const char *name,
                      const char *entryPoint,
                      std::span<RIBindlessDescriptorSet *const> externalSets = {},
                      std::span<const RIProgram::RIProgramBinding> bindings = {},
                      uint16_t pushConstantSize = 0,
                      uint16_t pushConstantMtlIndex = 0);

// Load a Slang vert+frag program. When `vertName == fragName` a single .spv blob
// is reused for both stages. `bindings` is the unified, combined-stage table
// (one entry per resource, with a `stages` bitmask). `pushConstantStages` names
// the stage(s) that read the push constant (e.g. RI_SHADER_STAGE_FRAGMENT).
void LoadSlangGraphics(RIDevice *device, RIProgram &prog,
                       cResources *resources, const char *vertName,
                       const char *fragName,
                       const char *vertEntryPoint = "vsMain",
                       const char *fragEntryPoint = "psMain",
                       std::span<RIBindlessDescriptorSet *const> externalSets = {},
                       std::span<const RIProgram::RIProgramBinding> bindings = {},
                       uint16_t pushConstantSize = 0,
                       uint32_t pushConstantStages = 0,
                       uint16_t pushConstantMtlIndex = 0);

} // namespace hpl

#endif
