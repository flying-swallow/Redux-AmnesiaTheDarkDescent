#ifndef HPL2_XESS_VULKAN_SUPPORT_H
#define HPL2_XESS_VULKAN_SUPPORT_H

#include <stdint.h>

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
#include <vulkan/vulkan.h>
#include <xess/xess.h>
#include <xess/xess_vk.h>
#endif

namespace hpl {

// Process/device-lifetime XeSS loader. The SDK module is kept loaded for the
// lifetime of this object so SDK-owned extension names and feature-chain
// structures remain valid while a Vulkan device or XeSS context uses them.
class cXessVulkanSupport {
public:
  cXessVulkanSupport();
  ~cXessVulkanSupport();

  bool IsAvailable() const { return m_available; }
  const char *UnavailableReason() const { return m_unavailableReason; }

  // Used by the Vulkan preflight when the selected instance/device cannot
  // satisfy XeSS. This does not affect ordinary Vulkan initialization.
  void SetUnavailable(const char *reason);

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
  typedef xess_result_t (*PFN_XessVKGetRequiredInstanceExtensions)(
      uint32_t *count, const char *const **names, uint32_t *minVkApiVersion);
  typedef xess_result_t (*PFN_XessVKGetRequiredDeviceExtensions)(
      VkInstance instance, VkPhysicalDevice physicalDevice, uint32_t *count,
      const char *const **names);
  typedef xess_result_t (*PFN_XessVKGetRequiredDeviceFeatures)(
      VkInstance instance, VkPhysicalDevice physicalDevice, void **features);
  typedef xess_result_t (*PFN_XessVKCreateContext)(
      VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
      xess_context_handle_t *context);
  typedef xess_result_t (*PFN_XessVKInit)(
      xess_context_handle_t context, const xess_vk_init_params_t *params);
  typedef xess_result_t (*PFN_XessVKExecute)(
      xess_context_handle_t context, VkCommandBuffer commandBuffer,
      const xess_vk_execute_params_t *params);
  typedef xess_result_t (*PFN_XessDestroyContext)(
      xess_context_handle_t context);
  typedef xess_result_t (*PFN_XessGetOptimalInputResolution)(
      xess_context_handle_t context, const xess_2d_t *outputResolution,
      xess_quality_settings_t qualitySettings,
      xess_2d_t *inputResolutionOptimal, xess_2d_t *inputResolutionMin,
      xess_2d_t *inputResolutionMax);
  typedef xess_result_t (*PFN_XessSetVelocityScale)(
      xess_context_handle_t context, float x, float y);
  typedef xess_result_t (*PFN_XessSetJitterScale)(
      xess_context_handle_t context, float x, float y);
  typedef xess_result_t (*PFN_XessSetExposureMultiplier)(
      xess_context_handle_t context, float scale);
  typedef xess_result_t (*PFN_XessGetProperties)(
      xess_context_handle_t context, const xess_2d_t *outputResolution,
      xess_properties_t *properties);
  typedef xess_result_t (*PFN_XessIsOptimalDriver)(
      xess_context_handle_t context);
  typedef xess_result_t (*PFN_XessGetVersion)(xess_version_t *version);
  typedef xess_result_t (*PFN_XessSetLoggingCallback)(
      xess_context_handle_t context, xess_logging_level_t loggingLevel,
      xess_app_log_callback_t loggingCallback);

  PFN_XessVKGetRequiredInstanceExtensions
  VKGetRequiredInstanceExtensions() const {
    return m_vkGetRequiredInstanceExtensions;
  }
  PFN_XessVKGetRequiredDeviceExtensions VKGetRequiredDeviceExtensions() const {
    return m_vkGetRequiredDeviceExtensions;
  }
  PFN_XessVKGetRequiredDeviceFeatures VKGetRequiredDeviceFeatures() const {
    return m_vkGetRequiredDeviceFeatures;
  }
  PFN_XessVKCreateContext VKCreateContext() const { return m_vkCreateContext; }
  PFN_XessVKInit VKInit() const { return m_vkInit; }
  PFN_XessVKExecute VKExecute() const { return m_vkExecute; }
  PFN_XessDestroyContext DestroyContext() const { return m_destroyContext; }
  PFN_XessGetOptimalInputResolution GetOptimalInputResolution() const {
    return m_getOptimalInputResolution;
  }
  PFN_XessSetVelocityScale SetVelocityScale() const {
    return m_setVelocityScale;
  }
  PFN_XessSetJitterScale SetJitterScale() const { return m_setJitterScale; }
  PFN_XessSetExposureMultiplier SetExposureMultiplier() const {
    return m_setExposureMultiplier;
  }
  PFN_XessGetProperties GetProperties() const { return m_getProperties; }
  PFN_XessIsOptimalDriver IsOptimalDriver() const {
    return m_isOptimalDriver;
  }
  PFN_XessGetVersion GetVersion() const { return m_getVersion; }
  PFN_XessSetLoggingCallback SetLoggingCallback() const {
    return m_setLoggingCallback;
  }
#endif

private:
  bool m_available;
  char m_unavailableReason[128];

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
  void *m_library;
  PFN_XessVKGetRequiredInstanceExtensions m_vkGetRequiredInstanceExtensions;
  PFN_XessVKGetRequiredDeviceExtensions m_vkGetRequiredDeviceExtensions;
  PFN_XessVKGetRequiredDeviceFeatures m_vkGetRequiredDeviceFeatures;
  PFN_XessVKCreateContext m_vkCreateContext;
  PFN_XessVKInit m_vkInit;
  PFN_XessVKExecute m_vkExecute;
  PFN_XessDestroyContext m_destroyContext;
  PFN_XessGetOptimalInputResolution m_getOptimalInputResolution;
  PFN_XessSetVelocityScale m_setVelocityScale;
  PFN_XessSetJitterScale m_setJitterScale;
  PFN_XessSetExposureMultiplier m_setExposureMultiplier;
  PFN_XessGetProperties m_getProperties;
  PFN_XessIsOptimalDriver m_isOptimalDriver;
  PFN_XessGetVersion m_getVersion;
  PFN_XessSetLoggingCallback m_setLoggingCallback;
#endif
};

cXessVulkanSupport &XessVulkanSupportInstance();

} // namespace hpl

#endif
