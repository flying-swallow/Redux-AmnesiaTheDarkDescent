#include "graphics/GraphicsTypes.h"
#include "graphics/RIBootstrap.h"
#include "graphics/HPLTexture.h"
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
void HPLTexture::HPLTexture_Delete(HPLTexture *texture) {
  // Defer GPU resource destruction to the next time this frame slot is
  // reused — by then the ring fence has signaled and the GPU is done with
  // anything that referenced this texture.
  RIBootstrap::FrameContext *cntx = RI.GetActiveSet();
  cntx->freelist.push_back(RIFree(texture->binding.vk.image.imageView));
  cntx->freelist.push_back(RIFree(texture->handle.vk.image));
  cntx->freelist.push_back(RIFree(texture->vk.vmaAlloc));
  delete texture;
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
  default:
    assert(false && "Unsupported texture format");
    break;
  }
  return RI_FORMAT_UNKNOWN;
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
	const RIFormatProps_s* prop,
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

void HPLTexture::setDebugName(const tWString& name) {
  std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
  std::string utf8 = converter.to_bytes(name);
  setDebugName(utf8.c_str());
}

void HPLTexture::setDebugName(const char* name) {
  assert(handle.vk.image);
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

bool HPLTexture::LoadBitmap(
                          RIBarrierImageHandle_s postBarrier,
                            cBitmap &bitmap, 
                            const BitmapLoadOptions &options) {
  width = bitmap.GetWidth();
  height = bitmap.GetHeight();
  depth = bitmap.GetDepth();
  RI_Format destFormat = hpl::to_image_supported_format(bitmap.GetPixelFormat());
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  const struct RIFormatProps_s *formatProps = GetRIFormatProps(destFormat);
  if (formatProps->blockWidth > 1) {
    info.flags |=
        VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT; // format can be used
                                                         // to create a view
                                                         // with an uncompressed
                                                         // format (1 texel
                                                         // covers 1 block)
  }
  info.mipLevels = options.use_mipmaps ? bitmap.GetNumOfMipMaps() : 1;
  info.arrayLayers = options.use_array ? bitmap.GetNumOfImages() : 1;
  if (options.use_cubemap) {
    info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; // allow cube maps
    if (options.use_array) {
      assert(bitmap.GetNumOfImages() % 6 == 0 &&
             "Cube map array must have a multiple of 6 images");
      info.arrayLayers = bitmap.GetNumOfImages();
    } else {
      assert(bitmap.GetNumOfImages() == 6 && "Cube map must have 6 images");
      info.arrayLayers = 6;
    }
  }
  info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                VK_IMAGE_CREATE_EXTENDED_USAGE_BIT; // typeless
  info.imageType = depth > 1
                       ? VK_IMAGE_TYPE_3D
                       : ((bitmap.GetWidth() == 1 || bitmap.GetHeight() == 1)
                              ? VK_IMAGE_TYPE_1D
                              : VK_IMAGE_TYPE_2D);
  info.format = RIFormatToVK(destFormat);
  info.extent.width = bitmap.GetWidth();
  info.extent.height = bitmap.GetHeight();
  info.extent.depth = bitmap.GetDepth();
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;

  uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
  info.pQueueFamilyIndices = queueFamilies;
  VK_ConfigureImageQueueFamilies(&info, RI.device.queues, RI_QUEUE_LEN,
                                 queueFamilies, RI_QUEUE_LEN);
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo memReqs = {0};
  memReqs.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	
	VkImageViewUsageCreateInfo usageInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
	VkImageSubresourceRange subresource = {
		VK_IMAGE_ASPECT_COLOR_BIT, 0, std::max<uint32_t>(info.mipLevels, 1), 0, 1,
	};
	VkImageViewCreateInfo createInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	if(!VK_WrapResult(vmaCreateImage(RI.device.vk.vmaAllocator, &info, &memReqs, &handle.vk.image, &vk.vmaAlloc, NULL))) {
	  return false;
	}

	usageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	createInfo.pNext = &usageInfo;
	createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	createInfo.format = RIFormatToVK( destFormat );
	createInfo.subresourceRange = subresource;
	createInfo.image = handle.vk.image;

	binding = {};
	binding.flags |= RI_VK_DESC_OWN_IMAGE_VIEW;
	binding.texture = &handle;
	binding.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	binding.vk.image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VK_WrapResult( vkCreateImageView( RI.device.vk.device, &createInfo, NULL, &binding.vk.image.imageView ) );
	RIFinalizeDescriptor( &RI.device, &binding );
	
	setDebugName(bitmap.GetFileName());
  RI_Format sourceFormat = from_hpl_format(bitmap.GetPixelFormat());
  const struct RIFormatProps_s* srcProps = GetRIFormatProps(sourceFormat);
  const struct RIFormatProps_s* destProps = GetRIFormatProps(destFormat);
#define MIP_REDUCE(s, mip) (std::max<uint32_t>(1u, (uint32_t)((s) >> (mip))))
  for (uint32_t arrIndex = 0; arrIndex < info.arrayLayers; arrIndex++) {
    for (uint32_t mipLevel = 0; mipLevel < info.mipLevels; mipLevel++) {
      const uint32_t w = MIP_REDUCE(info.extent.width, mipLevel);
      const uint32_t h = MIP_REDUCE(info.extent.height, mipLevel);

      const uint32_t srcSliceNum = (h / srcProps->blockHeight);
      const uint32_t srcRowPitch = (w / srcProps->blockWidth) * srcProps->stride;
      const auto& input = bitmap.GetData(arrIndex, mipLevel);
      if (input == NULL || input->mpData == NULL) {
        Warning("HPLTexture::LoadBitmap: missing source data for array %u mip %u\n",
                arrIndex, mipLevel);
        continue;
      }

      struct RIResourceTextureTransaction_s uploadDesc = {};
      uploadDesc.target = handle;
      uploadDesc.width = w;
      uploadDesc.height = h;
      uploadDesc.sliceNum = (h / destProps->blockHeight);
      uploadDesc.rowPitch = (w / destProps->blockWidth) * destProps->stride;
      uploadDesc.arrayOffset = arrIndex;
      uploadDesc.mipOffset = mipLevel;
      uploadDesc.x = 0;
      uploadDesc.y = 0;
      uploadDesc.depth = info.extent.depth;
      uploadDesc.format = destFormat;
      uploadDesc.vk.current_stage = VK_PIPELINE_STAGE_2_NONE;
      uploadDesc.vk.current_access = VK_ACCESS_2_NONE;
      uploadDesc.vk.current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
      uploadDesc.vk.post_stage = postBarrier.vk.stage;
      uploadDesc.vk.post_access = postBarrier.vk.access;
      uploadDesc.vk.post_layout = postBarrier.vk.layout;
      RI_ResourceBeginCopyTexture(&RI.device, &RI.uploader, &uploadDesc);

      // Iterate rows-of-blocks (== rows when blockHeight == 1).
      // alignSlicePitch is already bytes-per-slice, so the z term must not
      // be multiplied by alignRowPitch a second time.
      const size_t rowCount = uploadDesc.sliceNum;
      if (destProps->isCompressed) {
        assert(srcRowPitch == uploadDesc.rowPitch); // compressed formats must match
        assert(srcSliceNum == uploadDesc.sliceNum);
        for (size_t z = 0; z < info.extent.depth; ++z) {
          for (size_t slice = 0; slice < rowCount; ++slice) {
            const size_t srcRowStart = (srcRowPitch * slice) + (srcRowPitch * srcSliceNum * z);
            const size_t dstRowStart = (uploadDesc.alignRowPitch * slice) + (uploadDesc.alignSlicePitch * z);
            memcpy(&((uint8_t *)uploadDesc.mapped.data)[dstRowStart],
                   &input->mpData[srcRowStart], uploadDesc.rowPitch);
          }
        }
      } else {
        for (size_t z = 0; z < info.extent.depth; ++z) {
          for (size_t slice = 0; slice < rowCount; ++slice) {
            const size_t srcRowStart = (srcRowPitch * slice) + (srcRowPitch * srcSliceNum * z);
            const size_t dstRowStart = (uploadDesc.alignRowPitch * slice) + (uploadDesc.alignSlicePitch * z);
            memset(&((uint8_t *)uploadDesc.mapped.data)[dstRowStart], 255, uploadDesc.rowPitch);
            for (size_t column = 0; column < uploadDesc.width; ++column) {
              memcpy(&((uint8_t *)uploadDesc.mapped.data)[dstRowStart + (destProps->stride * column)],
                     &input->mpData[srcRowStart + (srcProps->stride * column)],
                     std::min(srcProps->stride, destProps->stride));
            }
          }
        }
      }
      RI_ResourceEndCopyTexture(&RI.device, &RI.uploader, &uploadDesc);
    }
  }
  return true;
}

//HPLTexture::HPLTexture() {
//
//}
//
//HPLTexture::HPLTexture(struct RIResourceUploader_s* upload,cBitmap& bitmap, const BitmapLoadOptions& options) {
//	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
//	info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT; // typeless
//	const struct RIFormatProps_s *formatProps = GetRIFormatProps( destFormat );
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
