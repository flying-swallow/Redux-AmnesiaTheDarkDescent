# NRD integration — engine API reference (HPL2 / RI Vulkan layer)

Scope: everything a native NVIDIA NRD integration needs from this engine's Vulkan
abstraction. Every claim is cited `file:line`. "not present" means the code has no
such thing — do not invent one.

Backend is Vulkan-only in practice: `volk.h` + `vk_mem_alloc.h` are pulled in by
`HPL2/core/include/graphics/RIPreamble.h:37` / `:40`, so any RI header transitively
gives you raw `Vk*` types and volk entry points. Handles are public members
(`RIDevice::vk.device` `HPL2/core/include/graphics/RIDevice.h:417`,
`RIDevice::vk.vmaAllocator` `:418`,
`RIDevice::physicalAdapter` `:408`,
`RIPhysicalAdapter::vk.physicalDevice` `:355`), and `RICmd::vk.cmd` is used directly
at call sites (e.g. `HPL2/core/sources/graphics/HybridRenderer.cpp:1958`). Dropping to
raw Vulkan mid-frame is an existing, sanctioned pattern.

---

## 1. `RIProgram` — can it drive an arbitrary external SPIR-V blob?

**Answer: yes, with one hard constraint (binding lookup is by reflected resource
NAME) and one hard limit (descriptor set index must be < 4).**

### Entry point is raw bytes, not a file path

```cpp
struct ModuleStage {                      // RIProgram.h:180
  uint8_t stage;                          // ProgramStages enum, RIProgram.h:167-179
  std::span<char> data;                   // RAW SPIR-V BYTES
  const char *entryPoint = "main";        // must match OpEntryPoint name exactly
};

void initialize(RIDevice *device, std::span<ModuleStage> init,             // RIProgram.h:197
                std::span<const VkDescriptorSetLayout> externalLayouts = {},
                const char *debugName = nullptr);
```

`initialize` copies `init.data` into `shaderBin[stage].buf`
(`HPL2/core/sources/graphics/RIProgram.cpp:743`). It never touches the filesystem.

`RIProgram::loadShaderStage(cFileSearcher*, const tString&)` (`RIProgram.h:205`,
impl `RIProgram.cpp:705-716`) is a *convenience* that reads a `.spv` off disk into a
`std::vector<char>`. `loadSlangCompute` in
`HPL2/core/sources/graphics/HybridRenderer.cpp:128-135` is just a lambda pairing
`loadShaderStage` + `ModuleStage` + `initialize`; it is not on the required path.
NRD hands back SPIR-V in memory → build `std::span<char>` over it and call
`initialize` directly.

### It DOES do SPIR-V reflection (SPIRV-Reflect), and bindings are NOT declared by the caller

`initialize` runs `spvReflectCreateShaderModule` per stage
(`RIProgram.cpp:747-749`) and derives, entirely from the blob:

- push-constant range (count, size, stage flags) — `RIProgram.cpp:750-789`
  (`spvReflectEnumeratePushConstantBlocks` at `:752` / `:762`); **more than one
  push-constant block per stage is a `FatalError`** (`RIProgram.cpp:757-760`).
- vertex input mask/formats (vertex stage only) — `RIProgram.cpp:791-799`.
- every descriptor set + binding → `VkDescriptorSetLayoutBinding` arrays, per-type
  counts on `DescriptorSetSlot`, and a `BindingReflection` record —
  `RIProgram.cpp:800-870`.
- then `vkCreateDescriptorSetLayout` per set (`RIProgram.cpp:975` / `:978`) and
  `vkCreatePipelineLayout` (`RIProgram.cpp:988`).

Hard limits:

- `static constexpr size_t DESCRIPTOR_SET_MAX = 4;` (`RIProgram.h:66`) and
  `assert(spv_reflection->set < programDescriptors.size())`
  (`RIProgram.cpp:806`). **A blob declaring set >= 4 is not supported.**
- multi-dim arrays asserted out: `assert(reflectionBinding->array.dims_count <= 1)`
  (`RIProgram.cpp:810`).

### The naming constraint (the real risk)

`BindingReflection.hash` is the hash of the *reflected resource name*:

```cpp
DescriptorBindingID reflID = CreateDescriptorBindingID(reflectionBinding->name); // RIProgram.cpp:816
reflc->set              = reflectionBinding->set;        // RIProgram.cpp:830
reflc->baseRegisterIndex = reflectionBinding->binding;   // RIProgram.cpp:831
```

`DescriptorBindingID::Create` does `hash_data(HASH_INITIAL_VALUE, name, strlen(name))`
(`HPL2/core/include/graphics/RIDescriptor.h:118-128`; free-function twin
`CreateDescriptorBindingID` at `:130-136`). `strlen(name)` is unguarded — a NULL
reflected name would be a null deref.

`bindDescriptors` resolves a caller's `DescriptorBinding` to a (set, binding) pair
**only** through that name hash: `findReflection` linear-searches `bindingReflection`
for an equal hash (`RIProgram.cpp:696-703`), and entries whose name does not resolve
are silently skipped by the `if( !refl || setIndex != refl->set || descriptor.isEmpty() ) continue;`
guards in both the hashing loop (`RIProgram.cpp:447-462`) and the write loop
(`RIProgram.cpp:508-518`).

Consequence for NRD: NRD's `DispatchDesc` identifies resources by *(set, binding)
numbers*, not names. To drive them through `RIProgram::bindDescriptors` you must know
the OpName string each NRD SPIR-V binding carries and pass it as the
`DescriptorBinding` name. There is **no** by-set/by-binding-number bind entry point —
not present.

### `bindComputePipeline`

```cpp
void bindComputePipeline(RIDevice*, RICmd*, hash_t pipelineHash,   // RIProgram.h:210
                         const char *debugName,
                         VkComputePipelineCreateInfo *pipelineCreateInfo);
```

Impl `RIProgram.cpp:143-179`. Caches by `pipelineHash`; on a miss it creates the
`VkShaderModule` from `shaderBin[PROGRAM_STAGE_COMPUTE]`, fills
`stage/stage.module/pName(entryPoint)/layout(=own pipelineLayout)`, calls
`vkCreateComputePipelines`, names it via `vkSetDebugUtilsObjectNameEXT`, destroys the
module, then `vkCmdBindPipeline(..., VK_PIPELINE_BIND_POINT_COMPUTE, ...)`. Caller
supplies only an (often empty) `VkComputePipelineCreateInfo`. Existing idiom:

```cpp
VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
prog.bindComputePipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                         kHash, "IndirectTemporalPass.cs", &ci);       // HybridRenderer.cpp:1452-1455, 1471-1473
```

One `RIProgram` = one shader binary set. NRD emits N distinct SPIR-V blobs → **N
`RIProgram` instances** (one per pipeline index), not N hashes on one program.

### `bindDescriptors`

```cpp
void bindDescriptors(RIDevice*, RICmd*, uint32_t frameIndex,       // RIProgram.h:241
                     DescriptorBinding *binding, size_t bindingCount,
                     VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS);

struct DescriptorBinding {                                          // RIProgram.h:97-114
  DescriptorBinding(const char *name, const RIDescriptor &desc,
                    uint32_t registerOffset = 0, bool optional = false);
  DescriptorBindingID handle;   // name hash
  uint32_t registerOffset;      // array element if reflected isArray, else binding += offset
  RIDescriptor descriptor;
  bool optional;                // exempt from the debug unwritten-binding warning
};
```

Impl `RIProgram.cpp:426-579`. Per set index 0..3: skips `isExternal` slots
(`:436-438`); hashes (reflection hash, registerOffset, descriptor cookie) of every
supplied non-empty binding into a set key (`:447-462`); looks the set up in the
per-slot allocator via `resolveDescriptorSetAlloc(device, &info->alloc, frameIndex, hash)`
(`:494`); on a cache miss builds `VkWriteDescriptorSet`s in batches of 32 and calls
`vkUpdateDescriptorSets` (`:495-562`, flush points `:517` and `:560`); finally
coalesces contiguous sets into `vkCmdBindDescriptorSets` calls (`:565` / `:574`).
Descriptor type comes from `RIDescriptor::type` via `ri_vk_BindlessDescriptorType`
(`RIProgram.cpp:15-27`, used at `:531`).

Debug builds log once per program per binding for any reflected, non-array binding
nobody wrote (`RIProgram.cpp:463-492`) — noisy but not fatal.

### `bindBindlessDescriptorSet` / `externalLayouts`

```cpp
void bindBindlessDescriptorSet(RICmd*, RIBindlessDescriptorSet*,   // RIProgram.h:249
                               uint32_t setIndex,
                               VkPipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS);
```

Impl `RIProgram.cpp:581-589`: a bare `vkCmdBindDescriptorSets` of
`bindless->vk.m_bindlessSet` at `setIndex` against this program's pipeline layout.
Requires that slot to have been registered external at `initialize` via
`externalLayouts` — the adopt-verbatim + `isExternal = true` branch is at
`RIProgram.cpp:957-966`; `bindDescriptors` then skips alloc/write/bind for it
(`RIProgram.cpp:436-438`).

Engine idiom (every hybrid pass): set 0 = the engine-global bindless set layout
`mpGraphics->globalset->m_bindlessSet.vk.m_bindlessSetLayout`
(`HybridRenderer.cpp:105-106`), passed as `externalLayouts` to every
`initialize` call, then bound per pass:

```cpp
prog.bindBindlessDescriptorSet(&mpGraphics->primary.cmds[0],
                               &mpGraphics->globalset->m_bindlessSet, 0,
                               VK_PIPELINE_BIND_POINT_COMPUTE);      // HybridRenderer.cpp:1474-1476
```

**NRD does not need this** — NRD shaders know nothing about set 0. Call
`initialize` with no `externalLayouts` and all four slots are program-owned.

### Push constants

Reflected, not declared: stage flags + size come from the blob
(`RIProgram.cpp:775-786`), exposed via `getPipelineLayout()` (`RIProgram.h:253`) and
`getPushConstantStageFlags()` (`RIProgram.h:247`). Two call idioms exist:

- wrapper: `RICmd::vk_d3d12_setPushConstants(RIDevice*, RIProgram&, uint32_t offset, uint32_t size, const void *data)`
  (`HPL2/core/include/graphics/RICommand.h:213`, impl
  `HPL2/core/sources/graphics/RIRenderer.cpp:2886-2894`) — pulls layout + stage flags
  from the program.
- raw: `vkCmdPushConstants(cmd.vk.cmd, prog.getPipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push)`
  (`HybridRenderer.cpp:1957-1960`).

### Verdict

An externally-supplied SPIR-V blob **can** be driven through `RIProgram` as-is
(pipeline creation, layout creation, descriptor allocation and binding all come free
from reflection) provided: sets < 4, ≤1 push-constant block per stage, no multi-dim
arrays, and the integration knows each binding's *reflected name*.

If NRD's blobs carry no usable names (stripped OpName), the fallback is the raw path,
using types the engine already exposes:

- `vkCreateDescriptorSetLayout` / `vkCreatePipelineLayout` / `vkCreateComputePipelines`
  / `vkCreateShaderModule` / `vkAllocateDescriptorSets` / `vkUpdateDescriptorSets` /
  `vkCmdBindDescriptorSets` / `vkCmdBindPipeline` / `vkCmdDispatch` — all reachable
  through volk from `RIPreamble.h:37`.
- handles: `device->vk.device` (`RIDevice.h:417`), `device->vk.vmaAllocator` (`:418`),
  `device->physicalAdapter.vk.physicalDevice` (`:355`, `:408`), `cmd->vk.cmd`.
- prior art for hand-rolled set layout + pool + set:
  `RIBindlessDescriptorSet::initialize` (`RIProgram.cpp:591-636`) and
  `RIBindlessDescriptorSet::writeDescriptors` (`RIProgram.cpp:638-694`);
  the struct itself is `RIProgram.h:20-63` with
  `Binding{binding, descriptorType, descriptorCount, stageFlags, flags}` (`:29-35`)
  and `WriteBinding{binding, arrayElement, RIDescriptor}` (`:37-41`). This is the
  cleanest existing scaffold to copy for an NRD-owned descriptor set.
- textures/views/barriers/constants below all stay usable unchanged in that path.

---

## 2. `RIDescriptor` — factories and set/binding association

Header `HPL2/core/include/graphics/RIDescriptor.h`.

```cpp
enum RIDescriptorType_e {                                    // RIDescriptor.h:22-29
  RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
  RI_DESCRIPTOR_TYPE_STORAGE_IMAGE,
  RI_DESCRIPTOR_TYPE_SAMPLER,
  RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
  RI_DESCRIPTOR_TYPE_STORAGE_BUFFER,
  RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE,
};

static RIDescriptor uniformBuffer(RIDevice*, RIBuffer*, uint64_t offset, uint64_t range);        // :173
static RIDescriptor storageBuffer(RIDevice*, RIBuffer*, uint64_t offset, uint64_t range);        // :176
static RIDescriptor sampledImage (RIDevice*, RITextureView*,
                                  RIResourceState_e state = RI_RESOURCE_STATE_SHADER_RESOURCE);  // :179
static RIDescriptor storageImage (RIDevice*, RITextureView*);                                    // :183
static RIDescriptor accelerationStructure(RIDevice*, RIAccelStructure*);                         // :185
static RIDescriptor sampler      (RIDevice*, RISampler*);                                        // :187
```

Implementations `HPL2/core/sources/graphics/RIRenderer.cpp:1606-1673`. Notes:

- `device` is unused in all of them (`(void)device;`).
- `storageImage` hardcodes `VK_IMAGE_LAYOUT_GENERAL` (`RIRenderer.cpp:1645-1653`).
- `sampledImage`'s `state` selects the layout via
  `ri_vk_RIResourceStateToImageLayout` (`RIRenderer.cpp:1637-1639`) — pass
  `RI_RESOURCE_STATE_GENERAL` when the image is a storage image being sampled (the
  denoiser chain does exactly this, `HybridRenderer.cpp:1481-1499`).
- `cookie` is the descriptor-set cache key, derived from the resource cookie + binding
  params; `cookie == 0` ⇒ `isEmpty()` ⇒ the binding is skipped
  (`RIDescriptor.h:190`, `ri_descriptor_cookie` `RIRenderer.cpp:1600-1604`).
- No combined-image-sampler: the engine uses separate sampled images + samplers
  (`RIDescriptor.h:19-21`).

**Set/binding association**: an `RIDescriptor` carries none. The (set, binding) pair
comes solely from `RIProgram`'s reflection of the shader resource NAME, matched at
bind time (see §1). `registerOffset` on `DescriptorBinding` is added to
`baseRegisterIndex` for non-array bindings (`RIProgram.cpp:524`), or used as
`dstArrayElement` for arrays (`RIProgram.cpp:522`).

---

## 3. Texture creation + the per-viewport pool

### `RITexture` / `RITextureView`

```cpp
struct RITextureDesc {                    // RITexture.h:47-58
  RITextureType_e type;   // RI_TEXTURE_1D/2D/3D           (RITexture.h:15)
  uint32_t format;        // RI_Format_e
  uint32_t width, height, depth;   // depth 0/1 = 2D
  uint32_t mipNum;        // 0 = 1
  uint32_t layerNum;      // 0 = 1
  uint32_t sampleCount;   // 0/1 = no MSAA
  uint32_t usage;         // RITextureUsageBits_e bitmask  (RITexture.h:17-26)
  uint32_t flags;         // RITextureFlagBits_e           (RITexture.h:37-44)
};
static RITexture RITexture::create(RIDevice*, const RITextureDesc&,
                                   std::optional<hash_t> hash = {});   // RITexture.h:65, impl RIRenderer.cpp:1692
void RITexture::dispose(RIDevice*);                                    // RITexture.h:68

struct RITextureViewDesc {                // RITextureView.h:39-46
  RITextureViewType_e viewType;           // RITextureView.h:16-36
  uint32_t format;  uint32_t baseMip, mipNum, baseLayer, layerNum;
};
static RITextureView RITextureView::create(RIDevice*, const RITexture*,
                                           const RITextureViewDesc&,
                                           std::optional<hash_t> hash = {}); // RITextureView.h:53, impl RIRenderer.cpp:1731
```

**Storage-capable request**: `desc.usage |= RI_USAGE_SHADER_RESOURCE_STORAGE`
(`RITexture.h:20`) → `VK_IMAGE_USAGE_STORAGE_BIT` in
`ri_vk_RITextureUsageToVK` (`RIRenderer.cpp:1636-1660`, storage bit at `:1645-1646`).
Every image is created with `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
VK_IMAGE_CREATE_EXTENDED_USAGE_BIT` (`RIRenderer.cpp:1699-1700`), allocated with VMA
(`vmaCreateImage`, `RIRenderer.cpp:1721-1722`), `initialLayout = UNDEFINED`
(`:1716`).

A storage *view* is either `RI_VIEWTYPE_SHADER_RESOURCE_2D` (what the whole hybrid
renderer uses — `RIDescriptor::storageImage` forces GENERAL layout regardless of view
type) or the explicit `RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D`
(`RITextureView.h:25`). All existing pool textures use the plain `_2D` view and bind
it both as `sampledImage(..., RI_RESOURCE_STATE_GENERAL)` and `storageImage(...)`.

### Format enum (`RI_Format_e`, `HPL2/core/include/graphics/RIFormat.h`)

`RI_FORMAT_R8_UNORM:18`, `RI_FORMAT_RGBA8_UNORM:30`, `RI_FORMAT_R16_SFLOAT:39`,
`RI_FORMAT_RG16_UNORM:40`, `RI_FORMAT_RG16_SFLOAT:44`, `RI_FORMAT_RGBA16_SFLOAT:50`,
`RI_FORMAT_R32_SFLOAT:53`, `RI_FORMAT_RGBA32_SFLOAT:62`.

Engine format constants: `cGraphics::VisibilityFormat = RI_FORMAT_RGBA32_UINT`
(`Graphics.h:226`), `DepthFormat = RI_FORMAT_D32_SFLOAT_S8_UINT` (`:229`),
`VelocityFormat = RI_FORMAT_RG16_SFLOAT` (`:231`),
`PogoColorFormat = RI_FORMAT_RGBA16_SFLOAT` (`:234`).

### The `HybridViewportState` pool

Declaration `HPL2/core/include/scene/Viewport.h:159-297`. All members are
`RISharedPointer<RITexture>` / `RISharedPointer<RITextureView>`; swapchain-indexed
arrays are `[RI_MAX_SWAPCHAIN_IMAGES]`, history ping-pongs are `[2]`.
Relevant existing entries: `renderTargetView:181`, `depthSampleView:188`,
`packedHitInfoView:198`, `velocityTexture:202` / `velocityView:203`,
`indirectKeyTexture/View:244/245`, `indirectResolvedTexture/View:256/257`,
`indirectSpecularResolvedTexture/View:267/268`.

Creation helpers (`HPL2/core/include/scene/Viewport.h:105` and `:113`, impl
`HPL2/core/sources/scene/Viewport.cpp:154-192` and `:194-229`):

```cpp
bool CreateViewportColorTexture(RIDevice*, uint32_t w, uint32_t h, RI_Format_e,
                                uint32_t usage,
                                RISharedPointer<RITexture>*, RISharedPointer<RITextureView>*,
                                const char *what);
bool CreateViewportAttachmentTexture(RIDevice*, uint32_t w, uint32_t h, RI_Format_e,
                                     uint32_t usage, RITextureViewType_e viewType,
                                     RISharedPointer<RITexture>*, RISharedPointer<RITextureView>*,
                                     const char *what);
```

Both build a `RITextureDesc{type=RI_TEXTURE_2D, format, width, height, usage}`, create
the image, then a view with `mipNum=1, layerNum=1`, and wrap both in
`RISharedPointer` (`Viewport.cpp:160-189`, `:201-228`).

**Exact idiom to add a new pool texture** — copy this (from
`HybridRenderer.cpp:377-383`, the `indirectResolved` entry):

```cpp
    CreateViewportAttachmentTexture(
        &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
        RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
            RI_USAGE_TRANSFER_DST,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &indirectResolvedTexture[i],
        &indirectResolvedView[i], "HybridViewportState.indirectResolved");
```

Plus, in the same commit, one line per handle in the destructor:

```cpp
    pGraphics->graphicsDefer.push(indirectResolvedTexture[i]);   // HybridRenderer.cpp:466
    pGraphics->graphicsDefer.push(indirectResolvedView[i]);      // HybridRenderer.cpp:479 region
```

Lifecycle: `cViewport::HybridViewportState::Update(cGraphics::FrameContext*, cVector2l)`
(`Viewport.h:172`, impl `HybridRenderer.cpp:237-437`) early-outs when the extent is
unchanged (`:246-250`), then does `*this = {};` (`:252`) which runs the destructor
(deferring every handle to `pGraphics->graphicsDefer`, `HybridRenderer.cpp:439-488`)
and move-constructs in place (`operator=`, `HybridRenderer.cpp:495-502`). History
invalidation flags are re-armed at the tail (`:432-436`:
`indirectLightingIndex = 0; indirectLightingInit = false; hasPrevCamera = false;`).
Deferred-destruction is therefore automatic — never call `dispose` on pool textures.

An NRD integration adding permanent/transient textures should either (a) add them to
`HybridViewportState` and follow this idiom exactly, or (b) own its own pool keyed on
extent, mirroring the same `graphicsDefer.push` discipline. `graphicsDefer` is
`cGraphics`-owned; `ReleaseViewportAttachmentTexture(tex, view)`
(`Viewport.cpp:231-241`) is the ad-hoc single-pair version.

---

## 4. Resource state transitions / barriers

Header `HPL2/core/include/graphics/RIBarrier.h`.

`enum RIResourceState_e` (`RIBarrier.h:20-41`) — OR-able access+layout bits:
`UNDEFINED=0`, `GENERAL`, `RENDER_TARGET`, `RENDER_TARGET_READ`, `DEPTH_WRITE`,
`DEPTH_READ`, `SHADER_RESOURCE`, `STORAGE_READ`, `STORAGE_WRITE`,
`UNORDERED_ACCESS(=STORAGE_READ|STORAGE_WRITE)`, `COPY_SRC`, `COPY_DST`, `PRESENT`,
`INDIRECT_ARGUMENT`, `VERTEX_BUFFER`, `INDEX_BUFFER`, `CONSTANT_BUFFER`,
`ACCEL_READ`, `ACCEL_WRITE`, `CLEAR_STORAGE`.

`enum RIStageBits_e` (`RIBarrier.h:46-59`): `RI_STAGE_NONE` (derive conservatively),
`VERTEX`, `FRAGMENT`, `COMPUTE`, `RAY_TRACING`, `DRAW_INDIRECT`, `COPY`, `BLIT`,
`CLEAR`, `ACCEL_BUILD`, `ALL_GRAPHICS`, `ALL_SHADER`.

`enum RIBarrierAspect_e` (`RIBarrier.h:61-66`): `COLOR` (default), `DEPTH`, `STENCIL`,
`DEPTH_STENCIL`.

Structs: `RITextureBarrier{texture, before, after, beforeStages, afterStages, aspect,
baseMip, mipCount, baseLayer, layerCount}` (`RIBarrier.h:68-90`; ctor at `:72-79`,
counts of 0 = REMAINING); `RIBufferBarrier` (`:92-108`); `RIMemoryBarrier{before,
after, beforeStages, afterStages}` (`:110-121`).

Layout mapping (`ri_vk_RIResourceStateToImageLayout`, `RIBarrier.h:139-166`):
**anything with GENERAL / STORAGE_READ / STORAGE_WRITE / CLEAR_STORAGE ⇒
`VK_IMAGE_LAYOUT_GENERAL`** — GENERAL wins over the optimal layouts, so a storage
image that is also sampled stays in GENERAL and needs no layout change between a
compute write and a compute sampled read. Access mapping at `:168-211`, stage
derivation at `:214-260`.

Commands (`HPL2/core/include/graphics/RICommand.h`):

```cpp
template <uint32_t MemN, uint32_t BufN, uint32_t TexN>              // RICommand.h:238-243
void vk_d3d12_resourceBarrier(uint32_t memoryBarrierNum, const RIMemoryBarrier*,
                              uint32_t bufferBarrierNum, const RIBufferBarrier*,
                              uint32_t textureBarrierNum, const RITextureBarrier*);
void vk_d3d12_memoryBarrier (const RIMemoryBarrier&);              // RICommand.h:315
void vk_d3d12_bufferBarrier (const RIBufferBarrier&);              // RICommand.h:318
void vk_d3d12_textureBarrier(const RITextureBarrier&);             // RICommand.h:321
template <uint32_t N>
void vk_d3d12_textureBarriers(uint32_t num, const RITextureBarrier*); // RICommand.h:325-329
void dispatch(RIDevice*, uint32_t gx, uint32_t gy, uint32_t gz);    // RICommand.h:160
void dispatchIndirect(RIDevice*, RIBuffer*, RIDeviceSize offset);   // RICommand.h:162
void clearStorageImage(RIDevice*, RITexture*, const float color[4]);// RICommand.h:194 (mip0/layer0, GENERAL)
```

`N == 0` on the templates routes the scratch array to the heap (`ScratchBuffer`
specialization, `RIBarrier.h:123-135`).

### The batch-transition example (the "array around 1330")

`HybridRenderer.cpp:1320-1368` — first-use init of the indirect chain:

```cpp
  if (!state.indirectLightingInit) {
    RITextureBarrier toGen[16] = {
        {state.indirectRadianceTexture[0].Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_CLEAR_STORAGE},
        ... 16 entries ...
    };
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<16>(16, toGen);   // :1357
    const float clr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (uint32_t i = 0; i < 16; ++i)
      mpGraphics->primary.cmds[0].clearStorageImage(&mpGraphics->device, toGen[i].texture, clr); // :1360-1361
    mpGraphics->primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_CLEAR_STORAGE,
         RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_STAGE_NONE, RI_STAGE_COMPUTE | RI_STAGE_RAY_TRACING});          // :1363-1366
    state.indirectLightingInit = true;
  } else {
    mpGraphics->primary.cmds[0].vk_d3d12_memoryBarrier(                     // :1371-1375
        {RI_RESOURCE_STATE_STORAGE_WRITE | RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_STAGE_COMPUTE | RI_STAGE_RAY_TRACING,
         RI_STAGE_COMPUTE | RI_STAGE_RAY_TRACING});
  }
```

### States for compute write vs compute read

- **compute write (storage image / UAV)**: `RI_RESOURCE_STATE_STORAGE_WRITE`
  (layout GENERAL). Bind via `RIDescriptor::storageImage(...)`.
- **compute read**:
  - sampled from a texture that stays GENERAL: bind
    `RIDescriptor::sampledImage(dev, view, RI_RESOURCE_STATE_GENERAL)`; the
    inter-pass barrier is `{STORAGE_WRITE → SHADER_RESOURCE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE}`
    as a plain memory barrier — **no image barrier, no layout change**
    (`HybridRenderer.cpp:1526-1529`, `:1584-1587`).
  - sampled from an image genuinely in SHADER_READ_ONLY (raster outputs, e.g.
    `gVelocity`): bind with the default `RI_RESOURCE_STATE_SHADER_RESOURCE`
    (`HybridRenderer.cpp:1500-1504`).

Whole-frame pattern for the denoiser textures: one-time `UNDEFINED → CLEAR_STORAGE`
+ clear, then they live in GENERAL forever and only memory barriers separate passes.
NRD's own texture pool can adopt exactly this.

---

## 5. Constant / uniform data — `UpdateFrameUBO`

```cpp
void cGraphics::UpdateFrameUBO(RIDescriptor *descriptor, void *data, size_t size);  // Graphics.h:334
```

Impl `HPL2/core/sources/graphics/Graphics.cpp:1058-1070`:

```cpp
  auto *activeSet = GetActiveSet();                               // frameSets[frameIndex % RI_NUMBER_FRAMES_FLIGHT]  (Graphics.h:330-332)
  RIBufferScratchAllocReq scratchReq =
      RIAllocBufferFromScratchAlloc(&device, &activeSet->uboScratchAlloc, size);
  memcpy((uint8_t*)scratchReq.pMappedAddress + scratchReq.bufferOffset, data, size);
  *descriptor = RIDescriptor::uniformBuffer(&device, &scratchReq.block.buffer,
                                            scratchReq.bufferOffset, size);
  RIFinishScrachReq(&device, &scratchReq);                        // vmaFlushAllocation, RIScratchAlloc.cpp:195-200
```

It is a **per-frame bump ring**, not a single buffer:

- pool `FrameContext::uboScratchAlloc` (`Graphics.h:237`), one per frame-in-flight.
- init: `blockSize = 256 * 128` (= 32768 B),
  `alignmentReq = device.physicalAdapter.constantBufferOffsetAlignment`,
  `alloc = RIUniformScratchAllocHandler` (`Graphics.cpp:299-303`; matching constants
  `RI_UNIFORM_SCRATCH_ALLLOC_SIZE` / `RI_UNIFORM_SCRATCH_REQ_ALIGNMENT (256)` at
  `RIScratchAlloc.h:5-6`).
- reset once per frame: `RIResetScratchAlloc(&device, &cntx->uboScratchAlloc)`
  (`Graphics.cpp:1034`) — recycles blocks, frees oversized one-shots
  (`RIScratchAlloc.cpp:88-104`).
- allocation (`RIScratchAlloc.cpp:130-193`): request is rounded up to `alignmentReq`;
  **a request larger than `blockSize` gets its own one-shot block** (`:139-150`), so
  there is no hard per-request size cap; otherwise it bump-allocates in the current
  block, chaining new/recycled blocks as needed (`:169-183`). Offsets are BDA-anchored
  to `alignmentReq` (`:157-167`).

**Verdict: an arbitrary per-dispatch byte blob CAN go through `UpdateFrameUBO`, tens
of times per frame. No separate ring buffer is needed.** Each call yields a fresh,
non-aliasing sub-allocation with its own `RIDescriptor` (cookie folds buffer identity
+ offset + range, `RIRenderer.cpp:1613-1616`), which is exactly how the à-trous loop
re-uploads per-iteration constants every iteration
(`HybridRenderer.cpp:1567-1572`). Constant blocks must be padded to std140 rules by
the caller (see `AtrousParamsHost { uint32_t stepSize; uint32_t pad[3]; }`,
`HybridRenderer.cpp:1558-1562`).

Caller idiom for a UBO binding (note: this one does NOT use `emplace_back`, because the
descriptor is an out-param):

```cpp
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gAtrous");
      mpGraphics->UpdateFrameUBO(&b.descriptor, &ap, sizeof(ap));
      ab.push_back(b);
    }                                                        // HybridRenderer.cpp:1567-1572
```

`RIPogoBuffer.cpp` is **not** relevant to constants: it is only the double-buffered
color attach pair (`RI_PogoBufferInit/Destroy/Toggle`,
`HPL2/core/sources/graphics/RIPogoBuffer.cpp:9-57`), whose textures are created with
`COLOR_ATTACHMENT | SHADER_RESOURCE | SHADER_RESOURCE_STORAGE | TRANSFER_SRC |
TRANSFER_DST` (`:22-24`) and toggled with two texture barriers (`:50-57`). Useful only
as another example of the create-texture idiom.

---

## 6. Samplers

```cpp
struct RISampler {                       // RIDescriptor.h:144-161
  void dispose(RIDevice*); bool isEmpty() const;
  union { struct { VkSampler sampler; } vk; };
  hash_t cookie;                          // 0 = uncreated
};
std::optional<RIDescriptor>
cGraphics::resolve_filter_descriptor(eTextureWrap wrapS, eTextureWrap wrapT,
                                     eTextureWrap wrapR, eTextureFilter filter);  // Graphics.h:324
```

Impl `Graphics.cpp:1072-1146`. Fills a `VkSamplerCreateInfo` with
`addressModeU/V/W = RI_VK_TextureWrap(wrap*)` (`:1078-1080`), filter mapping
(`:1081-1101`):

| `eTextureFilter` | min/mag | mipmapMode |
|---|---|---|
| `eTextureFilter_Nearest` | `VK_FILTER_NEAREST` | `VK_SAMPLER_MIPMAP_MODE_NEAREST` |
| `eTextureFilter_Bilinear` | `VK_FILTER_LINEAR` | `VK_SAMPLER_MIPMAP_MODE_NEAREST` |
| `eTextureFilter_Trilinear` | `VK_FILTER_LINEAR` | `VK_SAMPLER_MIPMAP_MODE_LINEAR` |

`maxLod = 16` (`:1102`). Cached by a collision-free mixed-radix key over
(wrapS, wrapT, wrapR, filter) into `cachedSamplers` (`Graphics.h:302`, 4*4*4*3 = 192
slots per the comment at `Graphics.h:318-319`); created on first use with
`vkCreateSampler` (`:1133-1136`); returns `RIDescriptor::sampler(&device, &sampler)`
(`:1142`). `eTextureWrap` / `eTextureFilter` are the engine enums
(`eTextureWrap_Repeat`, `eTextureFilter_Trilinear`, … — used at
`GlobalManagedSets.cpp:207-209`).

**Immutable / static samplers: not present.** Both layout builders set
`pImmutableSamplers = nullptr` (`RIProgram.cpp:602`) or leave the zero-initialized
binding (`VkDescriptorSetLayoutBinding bindings = {0};`, `RIProgram.cpp:848`). A sampler is an ordinary descriptor written
per frame like any other. Engine example — the one global material sampler:
declared as a `VK_DESCRIPTOR_TYPE_SAMPLER` bindless binding
(`GlobalManagedSets.cpp:73-75`), pool size 1 (`:131`), resolved once
(`:207-209`), written into the set (`:242-245`).

For NRD (which asks for a fixed set of point/linear × clamp/mirror samplers), build
them with `resolve_filter_descriptor` and bind them as normal `RIDescriptor` sampler
bindings, or create raw `VkSampler`s via volk on the raw path.

---

## 7. GPU profiling scope

```cpp
struct RIGpuScope {                                          // RIGpuProfiler.h:84-100
  RIGpuScope(RIGpuProfiler *profiler, RICmd *cmd, const char *name);
  ~RIGpuScope();   // non-copyable
};
```

RAII: `beginScope` writes a start timestamp + opens a `VK_EXT_debug_utils` label,
`endScope` closes both (`RIGpuProfiler.h:41-45`). Nestable. Cap:
`kMaxQueries = 256` ⇒ 128 scopes per frame across all viewports on the primary CB;
scopes past the cap time as 0 but stay labeled (`RIGpuProfiler.h:29-32`). Profiler
instance is `cGraphics::profiler` (`Graphics.h:281`).

Exact idiom used around the denoiser passes:

```cpp
      RIGpuScope _gsIndirectTemporal(&mpGraphics->profiler,
                                     &mpGraphics->primary.cmds[0],
                                     temporalScope);            // HybridRenderer.cpp:1468-1470

      RIGpuScope _gsIndirectAtrous(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                                   atrousScope);                // HybridRenderer.cpp:1541-1542
```

Other live scopes for reference: `"LightGrid"` `:877`, `"GBuffer"` `:922`,
`"VBufferPomBary"` `:1005`, `"DirectLighting"` `:1112`, `"DirectSpatialReuse"` `:1173`,
`"DirectAtrous"` `:1252`, `"PathTrace"` `:1387`, `"Decal"` `:1684`,
`"Composite"` `:1857`, `"Water"` `:2016`, `"Particle"` `:2200`,
`"TranslucentMesh"` `:2423`. NRD would add one outer scope (plus per-dispatch inner
ones only if the 128-scope budget allows — NRD emits tens of dispatches per frame).

---

## 8. The existing indirect denoiser chain — the seam

### `denoiseIndirectChannel`

Defined as a lambda inside a block at `HybridRenderer.cpp:1451-1609`:

```cpp
    auto denoiseIndirectChannel =                                  // :1461
        [&](const char *temporalScope, const char *atrousScope,
            RISharedPointer<RITextureView> *radiance,
            RISharedPointer<RITextureView> *atrous,
            RISharedPointer<RITextureView> *resolved) -> RITextureView *;
```

All three view params are `[2]` arrays indexed by `ilCur`/`ilPrev`
(`ilCur = state.indirectLightingIndex`, `ilPrev = ilCur ^ 1u`,
`HybridRenderer.cpp:1312-1313`). Returns the view the composite should sample.

Body, in order:

1. **Temporal half** (`:1467-1524`), scope `temporalScope`, program
   `m_indirectTemporal` (`IndirectTemporalPass.cs`), pipeline hash
   `kHash = hash_u32(HASH_INITIAL_VALUE, 0u)` (`:1455`), bindless set 0 bound
   (`:1474-1476`). Bindings (`:1477-1517`):
   | shader name | resource | how bound |
   |---|---|---|
   | `gIndirectCurrent` | `radiance[ilCur]` | `sampledImage(..., GENERAL)` |
   | `gIndirectKey` | `state.indirectKeyView[ilCur]` | `sampledImage(..., GENERAL)` |
   | `gIndirectHistory` | `resolved[ilPrev]` | `sampledImage(..., GENERAL)` |
   | `gIndirectKeyHistory` | `state.indirectKeyView[ilPrev]` | `sampledImage(..., GENERAL)` |
   | `gVelocity` | `state.velocityView[swapchainIndex]` | `sampledImage(..., SHADER_RESOURCE)` |
   | `gIndirectOut` | `radiance[ilCur]` (in-place) | `storageImage` |
   | `gIndirectResolvedOut` | `resolved[ilCur]` | `storageImage` |
   Dispatch `((w+15)/16, (h+15)/16, 1)` (`:1521-1522`).
2. Memory barrier `STORAGE_WRITE → SHADER_RESOURCE`, COMPUTE→COMPUTE (`:1526-1529`).
3. `resultView = radiance[ilCur]` — the fallback when the filter loop is disabled
   (`:1534`).
4. **À-trous half** (`:1535-1595`), scope `atrousScope`, program `m_directAtrous`
   (`DirectAtrousPass.cs`, shared with the direct chain), `kAtrousIterations = 5`
   (`amnesia/slang/Constants.h:398`). Per iteration `it`:
   `inView = (it==0) ? radiance[ilCur] : atrous[(it-1)&1]` (`:1544-1545`);
   `outView = (it==last) ? resolved[ilCur] : atrous[it&1]` (`:1546-1549`);
   bindings `gAtrous` (UBO `{stepSize = 1u<<it, pad[3]}`), `gAtrousIn` (sampled,
   GENERAL), `gDirectKey` = `state.indirectKeyView[ilCur]` (sampled, GENERAL),
   `gAtrousOut` (storage) — `:1558-1583`; dispatch 16×16 tiles (`:1580-1581`);
   trailing memory barrier `STORAGE_WRITE → SHADER_RESOURCE` (`:1586-1589`);
   `resultView = outView` (`:1590`).
5. `return resultView;` (`:1594`).

Upstream producer: `PathTracePass` (`:1385-1396` scope, `:1397-1445` bindings) writes
`gIndirectDiffuse = indirectRadianceView[ilCur]`, `gIndirectSpecular =
indirectSpecularView[ilCur]`, `gIndirectKeyOut = indirectKeyView[ilCur]`,
`gIndirectKeyExtra = indirectKeyExtraView[ilCur]` (`:1416-1436`), followed by
`{STORAGE_WRITE → SHADER_RESOURCE, RI_STAGE_RAY_TRACING → RI_STAGE_COMPUTE}`
(`:1442-1445`).

### Call sites (the seam)

```cpp
    indirectResultView = denoiseIndirectChannel(                  // :1602-1604
        "IndirectTemporal", "IndirectAtrous", state.indirectRadianceView,
        state.indirectAtrousView, state.indirectResolvedView);
    indirectSpecularResultView = denoiseIndirectChannel(          // :1605-1608
        "IndirectSpecularTemporal", "IndirectSpecularAtrous",
        state.indirectSpecularView, state.indirectSpecularAtrousView,
        state.indirectSpecularResolvedView);
```

The two out-params are declared at Draw scope, `HybridRenderer.cpp:1317-1318`:

```cpp
  RITextureView *indirectResultView = nullptr;
  RITextureView *indirectSpecularResultView = nullptr;
```

### Downstream consumption

Composite pass (`RIGpuScope "Composite"` at `:1857-1858`), bindings at `:1888-1895`:

```cpp
    bnd.emplace_back("gIndirectLighting",
                     RIDescriptor::sampledImage(&mpGraphics->device,
                                                indirectResultView,
                                                RI_RESOURCE_STATE_GENERAL));
    bnd.emplace_back("gIndirectSpecular",
                     RIDescriptor::sampledImage(&mpGraphics->device,
                                                indirectSpecularResultView,
                                                RI_RESOURCE_STATE_GENERAL));
```

(`gDirectLighting` next to them takes `directResultView` from the ReSTIR/DirectAtrous
chain, `:1902-1904`.) The ping-pong indices flip after the composite dispatch:
`state.directLightingIndex ^= 1u; state.indirectLightingIndex ^= 1u;`
(`HybridRenderer.cpp:1968-1969`).

### Seam description for an alternative denoiser

The contract is exactly: **given the path tracer's raw per-channel radiance +
key/velocity textures (all GENERAL, freshly barriered to SHADER_RESOURCE at `:1442`),
produce one `RITextureView*` per channel that is legal to sample in GENERAL layout at
`:1888-1895`.** Nothing between `:1451` and `:1609` is observed elsewhere except
(a) the two result pointers, and (b) `resolved[ilCur]` being valid history for next
frame (the temporal pass seeds it at `:1515-1517`, the last à-trous iteration
overwrites it at `:1547-1549`), and (c) the `indirectLightingIndex` flip at `:1969`.

A runtime toggle therefore drops in at `HybridRenderer.cpp:1600-1608`: branch on a
bool and assign `indirectResultView` / `indirectSpecularResultView` from either
`denoiseIndirectChannel(...)` or the NRD path. If the NRD path owns its own output
textures, it must also keep `state.indirectResolvedView[ilCur]` /
`state.indirectSpecularResolvedView[ilCur]` coherent (or the SVGF path will read a
stale history on toggle-back) and leave its outputs in GENERAL with a
`STORAGE_WRITE → SHADER_RESOURCE` memory barrier before the composite.

Inputs NRD would need that already exist per viewport:
`velocityView[swapchainIndex]` (RG16F, `Viewport.h:202-203`),
`depthSampleView[swapchainIndex]` (depth-aspect SRV, `Viewport.h:188`),
`packedHitInfoView[swapchainIndex]` (RGBA32UI V-buffer, `Viewport.h:198`),
`indirectKeyView[2]` (viewZ + normal.xyz, `Viewport.h:244-245`),
`indirectKeyExtraView[2]` (GGX alpha in .x, `Viewport.h:272-273`, created at
`HybridRenderer.cpp:407-412`, deferred at `:470` / `:483`). A separate normal/roughness packed in NRD's own format
is **not present** — it would have to be produced.

---

## 9. Runtime toggles — the existing idiom

Two patterns exist. Neither is a console cvar system; **a cvar/config-file registry
for renderer toggles is not present.**

### (a) Public bool on the `cGraphics` singleton + debug checkbox

Declaration:

```cpp
  bool allLightsCastShadows = true;                    // HPL2/core/include/graphics/Graphics.h:315
```

Read per frame into the per-frame UBO:

```cpp
  perFrame.allLightsCastShadows = mpGraphics->allLightsCastShadows ? 1u : 0u;   // HybridRenderer.cpp:692
```

(also read on the CPU side at `HPL2/core/sources/scene/World.cpp:534`, `:580`, `:607`,
`:998`), and consumed shader-side as `gPerFrame.allLightsCastShadows`
(`amnesia/slang/SceneTypes.slang:334`, used in `Material.slang:137`,
`DiffuseShading.slang:122/153/180`).

UI wiring in `amnesia/src/game/LuxDebugHandler.cpp`:

```cpp
		pCheckBox = mpGuiSet->CreateWidgetCheckBox(cVector3f(vGroupPos.x, vGroupPos.y + 8, vGroupPos.z), vSize, _W("Honor authored shadow flags"), pGroup);  // :1142
		pCheckBox->SetChecked(!hpl::Interface<cGraphics>::Get()->allLightsCastShadows, false);   // :1143
		pCheckBox->SetUserValue(19);                                                            // :1144
		pCheckBox->AddCallback(eGuiMessage_CheckChange, this, kGuiCallback(ChangeDebugText));    // :1145
		vGroupPos.y += 22;
```

and the dispatch on `GetUserValue()` inside one shared callback:

```cpp
bool cLuxDebugHandler::ChangeDebugText(iWidget* apWidget, const cGuiMessageData& aData)  // :1387
{
	int lNum = apWidget->GetUserValue();
	bool bActive = aData.mlVal == 1;
	if(lNum == 0)        mbShowFPS = bActive;
	...
	else if (lNum == 19) hpl::Interface<cGraphics>::Get()->allLightsCastShadows = !bActive;  // :1411
	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, ChangeDebugText);                            // :1415
```

Callback declared in the header as the three-line macro sandwich:

```cpp
	kGuiCallbackDeclaration(ChangeDebugText);                                   // LuxDebugHandler.h:99
	bool ChangeDebugText(iWidget* apWidget, const cGuiMessageData& aData);      // :100
	kGuiCallbackDeclarationEnd(ChangeDebugText);                                // :101
```

**To add one more toggle: pick an unused `SetUserValue(N)`, add the checkbox block
(4 lines + `vGroupPos.y += 22;`) next to `:1142-1146`, add one `else if (lNum == N)`
line near `:1411`, and declare the bool on `cGraphics` next to `Graphics.h:315`.** No
new callback function is needed.

### (b) Renderer-owned uint + combo box + push constant (the overlay mode)

```cpp
  void SetOverlay(int alOverlay) { m_overlayMode = (uint32_t)alOverlay; }   // HybridRenderer.h:40
  struct OverlayPushConstants { uint32_t overlayMode; };                    // HybridRenderer.h:136
  uint32_t m_overlayMode = kDefaultOverlayMode;                             // HybridRenderer.h:137
```

Pushed to the composite each frame:

```cpp
    static_assert(sizeof(OverlayPushConstants) == 4);
    const OverlayPushConstants push{m_overlayMode};
    vkCmdPushConstants(mpGraphics->primary.cmds[0].vk.cmd,
                       m_composite.getPipelineLayout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);   // HybridRenderer.cpp:1956-1960
```

Values are shared host/shader constants in `amnesia/slang/Constants.h:341-348`
(`SHARED_CONST uint kOverlayModeIndirectLighting = 0u; ... kOverlayModeShadowFlag = 6u;`
`kDefaultOverlayMode = kOverlayModeIndirectLighting;`), documented as "the debug UI
passes the combo-box item INDEX straight through as the mode value, so this enum must
stay dense and match the item order" (`Constants.h:333-340`).

UI: combo box built at `LuxDebugHandler.cpp:1129-1138` (`AddItem("None")` … 
`AddItem("Shadow Flag")`, `SetSelectedItem(-1)`), callback

```cpp
bool cLuxDebugHandler::ChangeEvaluationOverlay(iWidget* apWidget, const cGuiMessageData& aData)  // :1584
{
	cHybridRenderer* pHybridRenderer = static_cast<cHybridRenderer*>(gpBase->mpEngine->GetGraphics()->GetRenderer(eRenderer_Main));
	pHybridRenderer->SetOverlay(aData.mlVal);                                                    // :1587
	return true;
}
kGuiCallbackDeclaredFuncEnd(cLuxDebugHandler, ChangeEvaluationOverlay);                          // :1591
```

declared at `LuxDebugHandler.h:141-143`. Member handle `mpCBEvaluationOverlay`
(`LuxDebugHandler.h:160`, nulled at `LuxDebugHandler.cpp:93`).

Note the composite's overlay constants and the older `kOverlayMode*` block referenced
in the task are the same `Constants.h:341-348` block; a second, differently-numbered
overlay enum is **not present**.

**Recommendation for the NRD toggle**: pattern (a) — a `bool useNrdDenoiser` on
`cGraphics` next to `Graphics.h:315`, read at the seam
(`HybridRenderer.cpp:1600-1608`), with a checkbox at `LuxDebugHandler.cpp:1142-1146`
using a fresh `SetUserValue` id and one `else if` near `:1411`. It is CPU-side only,
so no shader constant or push-constant plumbing is required.

---

## Quick reference — files

| What | Where |
|---|---|
| Program / pipeline / descriptor binding | `HPL2/core/include/graphics/RIProgram.h`, `HPL2/core/sources/graphics/RIProgram.cpp` |
| Descriptor factories, `DescriptorBindingID`, `RISampler` | `HPL2/core/include/graphics/RIDescriptor.h`, impls `HPL2/core/sources/graphics/RIRenderer.cpp:1600-1673` |
| Texture / view | `HPL2/core/include/graphics/RITexture.h`, `RITextureView.h`, impls `RIRenderer.cpp:1692-1785` |
| Formats | `HPL2/core/include/graphics/RIFormat.h` |
| Barriers / states / stages | `HPL2/core/include/graphics/RIBarrier.h` |
| Command recording (dispatch, barriers, clears, push constants) | `HPL2/core/include/graphics/RICommand.h`, impls `RIRenderer.cpp` |
| Frame UBO ring | `HPL2/core/include/graphics/RIScratchAlloc.h`, `HPL2/core/sources/graphics/RIScratchAlloc.cpp`, `Graphics.cpp:1058-1070` |
| GPU scopes | `HPL2/core/include/graphics/RIGpuProfiler.h` |
| Per-viewport texture pool | `HPL2/core/include/scene/Viewport.h:159-297`, `HPL2/core/sources/scene/Viewport.cpp:154-241`, `HPL2/core/sources/graphics/HybridRenderer.cpp:238-502` |
| Denoiser seam | `HPL2/core/sources/graphics/HybridRenderer.cpp:1451-1609`, consumed `:1888-1895` |
| Global bindless set 0 | `HPL2/core/sources/graphics/GlobalManagedSets.cpp:30-174` |
| Debug toggles | `amnesia/src/game/LuxDebugHandler.cpp`, `amnesia/src/game/LuxDebugHandler.h`, `amnesia/slang/Constants.h:333-348` |
