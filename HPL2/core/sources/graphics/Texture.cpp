#include "graphics/GraphicsTypes.h"
#include "graphics/RIBootstrap.h"
#include "graphics/Texture.h"
#include "graphics/RIFormat.h"
#include "graphics/RIRenderer.h"
#include "graphics/RITypes.h"
#include "graphics/RIVK.h"

#include "graphics/Bitmap.h"
#include "system/LowLevelSystem.h"
#include "system/SystemTypes.h"

#include <codecvt>
#include <cassert>
#include <locale>
#include <vulkan/vulkan_core.h>

namespace hpl {
cTexture::cTexture(cTexture &&other) noexcept
    : handle(other.handle), format(other.format), view(other.view),
      width(other.width), height(other.height), depth(other.depth),
      mipNum(other.mipNum) {
  // Zero the source so its destructor disposes nothing (dispose is null-safe).
  other.handle = RITexture{};
  other.view = RITextureView{};
}

cTexture &cTexture::operator=(cTexture &&other) noexcept {
  if (this != &other) {
    // Release our own GPU resources before adopting the source's.
    view.dispose(&RI.device);
    handle.dispose(&RI.device);
    handle = other.handle;
    format = other.format;
    view = other.view;
    width = other.width;
    height = other.height;
    depth = other.depth;
    mipNum = other.mipNum;
    other.handle = RITexture{};
    other.view = RITextureView{};
  }
  return *this;
}

cTexture::~cTexture() {
  // Free the GPU resources directly — no freelist deferral needed. The owning
  // Image is parked (via a SharedResourcePin) in that frame set's resourceLink,
  // and BeginActiveSet waits the ring fence before clearing it, so by the time
  // the Image (and thus this texture) is freed the GPU is done with it.
  view.dispose(&RI.device);   // owned image view (was binding's inline view)
  handle.dispose(&RI.device);  // image + VMA allocation
}

RIDescriptor cTexture::descriptor() {
  return RIDescriptor::sampledImage(&RI.device, &view);
}

RI_Format from_hpl_format(ePixelFormat format) {
  switch (format) {
  case ePixelFormat_Alpha:
    return RI_FORMAT_R8_UNORM;
  case ePixelFormat_Luminance:
    return RI_FORMAT_R8_UNORM;
  case ePixelFormat_LuminanceAlpha:
    return RI_FORMAT_RG8_UNORM;
  case ePixelFormat_RGB: 
    return RI_FORMAT_RGB8_UNORM;
  case ePixelFormat_RGBA:
    return RI_FORMAT_RGBA8_UNORM;
  case ePixelFormat_BGRA:
    return RI_FORMAT_BGRA8_UNORM;
  case ePixelFormat_DXT1:
    return RI_FORMAT_BC1_RGBA_UNORM;
  case ePixelFormat_DXT2:
  case ePixelFormat_DXT3:
    return RI_FORMAT_BC2_RGBA_UNORM;
  case ePixelFormat_DXT4:
  case ePixelFormat_DXT5:
    return RI_FORMAT_BC3_RGBA_UNORM;
  case ePixelFormat_Depth16:
    return RI_FORMAT_D16_UNORM;
  case ePixelFormat_Depth24:
    return RI_FORMAT_D32_SFLOAT_S8_UINT;
  case ePixelFormat_Depth32:
    return RI_FORMAT_D32_SFLOAT;
  case ePixelFormat_Alpha16:
    return RI_FORMAT_R16_UNORM;
  case ePixelFormat_Luminance16:
    return RI_FORMAT_R16_UNORM;
  case ePixelFormat_LuminanceAlpha16:
    return RI_FORMAT_RG16_UNORM;
  case ePixelFormat_RGBA16:
    return RI_FORMAT_RGBA16_UNORM;
  case ePixelFormat_RGB16:
    return RI_FORMAT_RGB16_UNORM;
  case ePixelFormat_Alpha32:
    return RI_FORMAT_R32_SFLOAT;
  case ePixelFormat_Luminance32:
    return RI_FORMAT_R32_SFLOAT;
  case ePixelFormat_LuminanceAlpha32:
    return RI_FORMAT_RG32_SFLOAT;
  case ePixelFormat_RGBA32:
    return RI_FORMAT_RGBA32_SFLOAT;
  case ePixelFormat_BGR:
    return RI_FORMAT_BGR8_UNORM;
  case ePixelFormat_RGB32:
    return RI_FORMAT_RGB32_SFLOAT;
  default:
    assert(false && "Unsupported texture format");
    break;
  }
  return RI_FORMAT_UNKNOWN;
}


// Upgrade a UNORM color format to its sRGB variant so the sampler decodes
// sRGB→linear on read. Formats with no sRGB sibling (single-channel, 16-bit,
// floats, depth) pass through unchanged.
static RI_Format to_srgb_format(RI_Format f) {
  switch (f) {
  case RI_FORMAT_RGBA8_UNORM: return RI_FORMAT_RGBA8_SRGB;
  case RI_FORMAT_BGRA8_UNORM: return RI_FORMAT_BGRA8_SRGB;
  case RI_FORMAT_BC1_RGBA_UNORM: return RI_FORMAT_BC1_RGBA_SRGB;
  case RI_FORMAT_BC2_RGBA_UNORM: return RI_FORMAT_BC2_RGBA_SRGB;
  case RI_FORMAT_BC3_RGBA_UNORM: return RI_FORMAT_BC3_RGBA_SRGB;
  case RI_FORMAT_BC7_RGBA_UNORM: return RI_FORMAT_BC7_RGBA_SRGB;
  default: return f;
  }
}

RI_Format to_image_supported_format(ePixelFormat format) {
  switch (format) {
  case ePixelFormat_Alpha:
  case ePixelFormat_Luminance:
    return RI_FORMAT_R8_UNORM;
  case ePixelFormat_LuminanceAlpha:
    return RI_FORMAT_RG8_UNORM;
  case ePixelFormat_RGB: // generally not supported most hardware does not
                         // support 24 bit formats
  case ePixelFormat_RGBA:
    return RI_FORMAT_RGBA8_UNORM;
  case ePixelFormat_BGRA:
    return RI_FORMAT_BGRA8_UNORM;
  case ePixelFormat_DXT1:
    return RI_FORMAT_BC1_RGBA_UNORM;
  case ePixelFormat_DXT2:
  case ePixelFormat_DXT3:
    return RI_FORMAT_BC2_RGBA_UNORM;
  case ePixelFormat_DXT4:
  case ePixelFormat_DXT5:
    return RI_FORMAT_BC3_RGBA_UNORM;
  case ePixelFormat_Depth16:
    return RI_FORMAT_D16_UNORM;
  case ePixelFormat_Depth24:
    return RI_FORMAT_D32_SFLOAT_S8_UINT;
  case ePixelFormat_Depth32:
    return RI_FORMAT_D32_SFLOAT;
  case ePixelFormat_Alpha16:
  case ePixelFormat_Luminance16:
    return RI_FORMAT_R16_UNORM;
  case ePixelFormat_LuminanceAlpha16:
    return RI_FORMAT_RG16_UNORM;
  case ePixelFormat_RGBA16:
  case ePixelFormat_RGB16:
    return RI_FORMAT_RGBA16_UNORM;
  case ePixelFormat_Alpha32:
  case ePixelFormat_Luminance32:
    return RI_FORMAT_R32_SFLOAT;
  case ePixelFormat_LuminanceAlpha32:
    return RI_FORMAT_RG32_SFLOAT;
  case ePixelFormat_RGBA32:
    return RI_FORMAT_RGBA32_SFLOAT;
  case ePixelFormat_BGR:
    return RI_FORMAT_BGRA8_UNORM;
  default:
    assert(false && "Unsupported texture format");
    break;
  }
  return RI_FORMAT_UNKNOWN;
}

static inline bool GetSurfaceInfo(
	uint32_t width,
	uint32_t height,
	const RIFormatProps* prop,
	uint32_t* outNumBytes,
	uint32_t* outRowBytes,
	uint32_t* outNumRows)
{

	uint64_t numBytes = 0;
	uint64_t rowBytes = 0;
	uint64_t numRows = 0;
  
	uint32_t bpp = prop->stride * 8;
	bool compressed = prop->isCompressed;
	if (compressed)
	{
		uint32_t blockWidth = prop->blockWidth;
		uint32_t blockHeight = prop->blockHeight;
		uint32_t numBlocksWide = 0;
		uint32_t numBlocksHigh = 0;
		if (width > 0)
		{
			numBlocksWide = std::max(1U, (width + (blockWidth - 1)) / blockWidth);
		}
		if (height > 0)
		{
			numBlocksHigh = std::max(1u, (height + (blockHeight - 1)) / blockHeight);
		}

		rowBytes = numBlocksWide * (bpp >> 3);
		numRows = numBlocksHigh;
		numBytes = rowBytes * numBlocksHigh;
	}
	else
	{
		if (!bpp)
			return false;

		rowBytes = (uint64_t(width) * bpp + 7u) / 8u; // round up to nearest byte
		numRows = uint64_t(height);
		numBytes = rowBytes * height;
	}

	if (numBytes > UINT32_MAX || rowBytes > UINT32_MAX || numRows > UINT32_MAX) //-V560
		return false;

	if (outNumBytes)
	{
		*outNumBytes = (uint32_t)numBytes;
	}
	if (outRowBytes)
	{
		*outRowBytes = (uint32_t)rowBytes;
	}
	if (outNumRows)
	{
		*outNumRows = (uint32_t)numRows;
	}

	return true;
}

void cTexture::setDebugName(const tWString& name) {
  std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
  std::string utf8 = converter.to_bytes(name);
  setDebugName(utf8.c_str());
}

void cTexture::setDebugName(const char* name) {
  assert(!handle.isEmpty());
	if(vkSetDebugUtilsObjectNameEXT){
		VkDebugUtilsObjectNameInfoEXT debugName = { 
			VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, 
			NULL, 
			VK_OBJECT_TYPE_IMAGE, 
			(uint64_t)handle.vk.image, 
			name 
		};
		VK_WrapResult( vkSetDebugUtilsObjectNameEXT( RI.device.vk.device, &debugName ) );
	}

}

// Stage-copies one (arrIndex, mipLevel) subresource worth of `srcData` into
// `target` through the resource uploader, converting from the bitmap's pixel
// layout (sourceFormat) to destFormat. current/post states parameterize the
// uploader's barriers: UNDEFINED for the first fill of a fresh image,
// SHADER_RESOURCE for an in-place re-upload of a fragment-sampled texture.
static void __UploadBitmapSubresource(struct RITexture &target,
                                      const unsigned char *srcData,
                                      RI_Format sourceFormat, RI_Format destFormat,
                                      uint32_t arrIndex, uint32_t mipLevel,
                                      uint32_t w, uint32_t h, uint32_t depth,
                                      enum RIResourceState_e currentState,
                                      uint32_t currentStages,
                                      enum RIResourceState_e postState,
                                      uint32_t postStages) {
  const struct RIFormatProps *srcProps = GetRIFormatProps(sourceFormat);
  const struct RIFormatProps *destProps = GetRIFormatProps(destFormat);

  // Compressed mip levels below blockWidth/blockHeight still occupy one
  // full block. Without the max() clamp small mips give rowPitch == 0,
  // which triggers a divide-by-zero in RI_ResourceEndCopyTexture.
  const uint32_t srcSliceNum = std::max<uint32_t>(1u, h / srcProps->blockHeight);
  const uint32_t srcRowPitch = std::max<uint32_t>(1u, w / srcProps->blockWidth) * srcProps->stride;

  struct RIResourceTextureTransaction uploadDesc = {};
  uploadDesc.target = target;
  uploadDesc.width = w;
  uploadDesc.height = h;
  uploadDesc.sliceNum = std::max<uint32_t>(1u, h / destProps->blockHeight);
  uploadDesc.rowPitch = std::max<uint32_t>(1u, w / destProps->blockWidth) * destProps->stride;
  uploadDesc.arrayOffset = arrIndex;
  uploadDesc.mipOffset = mipLevel;
  uploadDesc.x = 0;
  uploadDesc.y = 0;
  uploadDesc.depth = depth;
  uploadDesc.format = destFormat;
  uploadDesc.currentState = currentState;
  uploadDesc.currentStages = currentStages;
  uploadDesc.postState = postState;
  uploadDesc.postStages = postStages;
  RI_ResourceBeginCopyTexture(&RI.device, &RI.uploader, &uploadDesc);

  // Iterate rows-of-blocks (== rows when blockHeight == 1).
  // alignSlicePitch is already bytes-per-slice, so the z term must not
  // be multiplied by alignRowPitch a second time.
  const size_t rowCount = uploadDesc.sliceNum;
  if (destProps->isCompressed) {
    assert(srcRowPitch == uploadDesc.rowPitch); // compressed formats must match
    assert(srcSliceNum == uploadDesc.sliceNum);
    for (size_t z = 0; z < depth; ++z) {
      for (size_t slice = 0; slice < rowCount; ++slice) {
        const size_t srcRowStart = (srcRowPitch * slice) + (srcRowPitch * srcSliceNum * z);
        const size_t dstRowStart = (uploadDesc.alignRowPitch * slice) + (uploadDesc.alignSlicePitch * z);
        memcpy(&((uint8_t *)uploadDesc.mapped.data)[dstRowStart],
               &srcData[srcRowStart], uploadDesc.rowPitch);
      }
    }
  } else {
    for (size_t z = 0; z < depth; ++z) {
      for (size_t slice = 0; slice < rowCount; ++slice) {
        const size_t srcRowStart = (srcRowPitch * slice) + (srcRowPitch * srcSliceNum * z);
        const size_t dstRowStart = (uploadDesc.alignRowPitch * slice) + (uploadDesc.alignSlicePitch * z);
        memset(&((uint8_t *)uploadDesc.mapped.data)[dstRowStart], 255, uploadDesc.rowPitch);
        for (size_t column = 0; column < uploadDesc.width; ++column) {
          memcpy(&((uint8_t *)uploadDesc.mapped.data)[dstRowStart + (destProps->stride * column)],
                 &srcData[srcRowStart + (srcProps->stride * column)],
                 std::min(srcProps->stride, destProps->stride));
        }
      }
    }
  }
  RI_ResourceEndCopyTexture(&RI.device, &RI.uploader, &uploadDesc);
}

bool cTexture::LoadBitmap(
                          enum RIResourceState_e postState, uint32_t postStages,
                            cBitmap &bitmap,
                            const BitmapLoadOptions &options) {
  width = bitmap.GetWidth();
  height = bitmap.GetHeight();
  depth = bitmap.GetDepth();
  RI_Format destFormat = hpl::to_image_supported_format(bitmap.GetPixelFormat());
  if (options.sRGB) {
    destFormat = to_srgb_format(destFormat);
  }
  format = destFormat; // remembered so material setup can probe channel count
  const struct RIFormatProps *formatProps = GetRIFormatProps(destFormat);
  uint32_t texFlags = RI_TEXTURE_FLAG_NONE;
  if (formatProps->blockWidth > 1) {
    // block-compressed format can be viewed with an uncompressed format (1
    // texel covers 1 block)
    texFlags |= RI_TEXTURE_FLAG_BLOCK_TEXEL_VIEW_COMPATIBLE;
  }
  uint32_t mipLevels = options.use_mipmaps ? bitmap.GetNumOfMipMaps() : 1;
  uint32_t arrayLayers = options.use_array ? bitmap.GetNumOfImages() : 1;
  if (options.use_cubemap) {
    texFlags |= RI_TEXTURE_FLAG_CUBE_COMPATIBLE; // allow cube maps
    if (options.use_array) {
      assert(bitmap.GetNumOfImages() % 6 == 0 &&
             "Cube map array must have a multiple of 6 images");
      arrayLayers = bitmap.GetNumOfImages();
    } else {
      assert(bitmap.GetNumOfImages() == 6 && "Cube map must have 6 images");
      arrayLayers = 6;
    }
  }
  // 1xN / Nx1 sources stay RI_TEXTURE_2D (a 1-tall or 1-wide 2D image is
  // valid). Every texture created here is bound into the `textures_2d[]`
  // bindless array, whose shader OpTypeImage is 2D; a 1D view in that slot
  // fails the descriptor view-type match at submit
  // (VUID-vkCmdDraw-viewType-07752).
  const enum RITextureType_e texType =
      depth > 1 ? RI_TEXTURE_3D : RI_TEXTURE_2D;

  RITextureDesc desc = {};
  desc.type = texType;
  desc.format = destFormat;
  desc.width = bitmap.GetWidth();
  desc.height = bitmap.GetHeight();
  desc.depth = bitmap.GetDepth();
  desc.mipNum = mipLevels;
  desc.layerNum = arrayLayers;
  desc.usage = RI_USAGE_SHADER_RESOURCE | RI_USAGE_TRANSFER_SRC |
               RI_USAGE_TRANSFER_DST;
  desc.flags = texFlags;
  handle = RITexture::create(&RI.device, desc);
  if (handle.isEmpty()) {
    return false;
  }

	RITextureViewDesc viewDesc = {};
	// View type must match texType and the bindless array's 2D OpTypeImage.
	// texType is only ever 3D (depth>1) or 2D here, so 1xN/Nx1 textures get a
	// 2D view that the textures_2d[] array accepts.
	if (options.use_cubemap) {
		viewDesc.viewType = options.use_array ? RI_VIEWTYPE_SHADER_RESOURCE_CUBE_ARRAY : RI_VIEWTYPE_SHADER_RESOURCE_CUBE;
	} else if (texType == RI_TEXTURE_3D) {
		viewDesc.viewType = RI_VIEWTYPE_SHADER_RESOURCE_3D;
	} else {
		viewDesc.viewType = options.use_array ? RI_VIEWTYPE_SHADER_RESOURCE_2D_ARRAY : RI_VIEWTYPE_SHADER_RESOURCE_2D;
	}
	viewDesc.format = destFormat;
	viewDesc.mipNum = std::max<uint32_t>(mipLevels, 1);
	viewDesc.layerNum = arrayLayers;
	view = RITextureView::create( &RI.device, &handle, viewDesc );
	
	setDebugName(bitmap.GetFileName());
  RI_Format sourceFormat = from_hpl_format(bitmap.GetPixelFormat());
#define MIP_REDUCE(s, mip) (std::max<uint32_t>(1u, (uint32_t)((s) >> (mip))))
  for (uint32_t arrIndex = 0; arrIndex < arrayLayers; arrIndex++) {
    for (uint32_t mipLevel = 0; mipLevel < mipLevels; mipLevel++) {
      const uint32_t w = MIP_REDUCE(desc.width, mipLevel);
      const uint32_t h = MIP_REDUCE(desc.height, mipLevel);

      const auto& input = bitmap.GetData(arrIndex, mipLevel);
      if (input == NULL || input->mpData == NULL) {
        Warning("cTexture::LoadBitmap: missing source data for array %u mip %u\n",
                arrIndex, mipLevel);
        continue;
      }

      __UploadBitmapSubresource(handle, input->mpData, sourceFormat, destFormat,
                                arrIndex, mipLevel, w, h, desc.depth,
                                RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_NONE,
                                postState, postStages);
    }
  }
  return true;
}

bool cTexture::UpdateBitmap(cBitmap &bitmap) {
  // Size and format must match the original LoadBitmap — this only refreshes
  // the pixels of mip 0 / layer 0 (the SetRawData replacement for procedural
  // textures like the color picker's box/slider).
  assert(!handle.isEmpty());
  assert(bitmap.GetWidth() == width && bitmap.GetHeight() == height &&
         bitmap.GetDepth() == depth);
  assert(to_image_supported_format(bitmap.GetPixelFormat()) == format);

  const auto &input = bitmap.GetData(0, 0);
  if (input == NULL || input->mpData == NULL) {
    Warning("cTexture::UpdateBitmap: missing source data\n");
    return false;
  }

  RI_Format sourceFormat = from_hpl_format(bitmap.GetPixelFormat());
  __UploadBitmapSubresource(handle, input->mpData, sourceFormat, (RI_Format)format,
                            0, 0, width, height, depth,
                            RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
                            RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);
  return true;
}

//cTexture::cTexture() {
//
//}
//
//cTexture::cTexture(struct RIResourceUploader* upload,cBitmap& bitmap, const BitmapLoadOptions& options) {
//	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
//	info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT; // typeless
//	const struct RIFormatProps *formatProps = GetRIFormatProps( destFormat );
//	//bitmap.GetPixelFormat()
//	////if( formatProps->blockWidth > 1 )
//	////	info.flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT; // format can be used to create a view with an uncompressed format (1 texel covers 1 block)
//	////if( image->flags & IT_CUBEMAP )
//	////	info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; // allow cube maps
//	//info.imageType = VK_IMAGE_TYPE_2D;
//	//info.format = RIFormatToVK( destFormat );
//	//info.extent.width = bitmap.GetWidth();
//	//info.extent.height = bitmap.GetHeight();
//	//info.extent.depth = 1;
//	//info.mipLevels = image->mipNum;
//	//info.arrayLayers = ( image->flags & IT_CUBEMAP ) ? 6 : 1;
//	//info.samples = 1;
//	//info.tiling = VK_IMAGE_TILING_OPTIMAL;
//	//info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
//
//	//info.pQueueFamilyIndices = queueFamilies;
//	//VK_ConfigureImageQueueFamilies( &info, rsh.device.queues, RI_QUEUE_LEN, queueFamilies, RI_QUEUE_LEN );
//	//info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
//	//
//	//VmaAllocationCreateInfo memCreateInfo = { 0 };
//	//memCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
//	//if(!VK_WrapResult(vmaCreateImage(rsh.device.vk.vmaAllocator, &info, &memCreateInfo, &image->handle.vk.image, &image->vk.vmaAlloc, NULL))) {
//	//	ri.Com_Printf( S_COLOR_YELLOW "Failed to Create Image: %s\n", image->name.buf );
//	//	__FreeImage( image );
//	//	image = NULL;
//	//	return NULL;
//	//}
//
//} 
//

} // namespace hpl
