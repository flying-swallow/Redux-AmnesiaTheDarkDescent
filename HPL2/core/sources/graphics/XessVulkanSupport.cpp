#include "graphics/XessVulkanSupport.h"

#include <stdio.h>
#include <string.h>

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
#ifdef _WIN32
#include <windows.h>
#endif
#endif

namespace hpl {

namespace {
void CopyUnavailableReason(char *destination, const char *reason) {
  if (!reason || !reason[0])
    reason = "unspecified reason";
  strncpy(destination, reason, 127);
  destination[127] = '\0';
}
} // namespace

cXessVulkanSupport::cXessVulkanSupport()
    : m_available(false), m_unavailableReason{} {
  CopyUnavailableReason(m_unavailableReason, "XeSS is unavailable");

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
  m_library = NULL;
  m_vkGetRequiredInstanceExtensions = NULL;
  m_vkGetRequiredDeviceExtensions = NULL;
  m_vkGetRequiredDeviceFeatures = NULL;
  m_vkCreateContext = NULL;
  m_vkInit = NULL;
  m_vkExecute = NULL;
  m_destroyContext = NULL;
  m_getOptimalInputResolution = NULL;
  m_setVelocityScale = NULL;
  m_setJitterScale = NULL;
  m_setExposureMultiplier = NULL;
  m_getProperties = NULL;
  m_isOptimalDriver = NULL;
  m_getVersion = NULL;
  m_setLoggingCallback = NULL;

#ifdef _WIN32
  m_library = (void *)LoadLibraryA("libxess.dll");
  if (!m_library) {
    CopyUnavailableReason(m_unavailableReason,
                          "XeSS DLL libxess.dll could not be loaded");
    return;
  }

#define XESS_LOAD_SYMBOL(member, name)                                         \
  member = reinterpret_cast<decltype(member)>(GetProcAddress(                 \
      static_cast<HMODULE>(m_library), name));                                 \
  if (!member) {                                                               \
    char reason[128];                                                          \
    snprintf(reason, sizeof(reason), "XeSS DLL symbol %s is missing", name);  \
    CopyUnavailableReason(m_unavailableReason, reason);                        \
    return;                                                                    \
  }

  XESS_LOAD_SYMBOL(m_vkGetRequiredInstanceExtensions,
                   "xessVKGetRequiredInstanceExtensions");
  XESS_LOAD_SYMBOL(m_vkGetRequiredDeviceExtensions,
                   "xessVKGetRequiredDeviceExtensions");
  XESS_LOAD_SYMBOL(m_vkGetRequiredDeviceFeatures,
                   "xessVKGetRequiredDeviceFeatures");
  XESS_LOAD_SYMBOL(m_vkCreateContext, "xessVKCreateContext");
  XESS_LOAD_SYMBOL(m_vkInit, "xessVKInit");
  XESS_LOAD_SYMBOL(m_vkExecute, "xessVKExecute");
  XESS_LOAD_SYMBOL(m_destroyContext, "xessDestroyContext");
  XESS_LOAD_SYMBOL(m_getOptimalInputResolution,
                   "xessGetOptimalInputResolution");
  XESS_LOAD_SYMBOL(m_setVelocityScale, "xessSetVelocityScale");
  XESS_LOAD_SYMBOL(m_setJitterScale, "xessSetJitterScale");
  XESS_LOAD_SYMBOL(m_setExposureMultiplier, "xessSetExposureMultiplier");
  XESS_LOAD_SYMBOL(m_getProperties, "xessGetProperties");
  XESS_LOAD_SYMBOL(m_isOptimalDriver, "xessIsOptimalDriver");
  XESS_LOAD_SYMBOL(m_getVersion, "xessGetVersion");
  XESS_LOAD_SYMBOL(m_setLoggingCallback, "xessSetLoggingCallback");

#undef XESS_LOAD_SYMBOL
  m_available = true;
  m_unavailableReason[0] = '\0';
#else
  CopyUnavailableReason(m_unavailableReason,
                        "XeSS is only available on Windows");
#endif
#else
  CopyUnavailableReason(m_unavailableReason,
                        "XeSS support is not compiled into this build");
#endif
}

cXessVulkanSupport::~cXessVulkanSupport() {
#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE && defined(_WIN32)
  if (m_library)
    FreeLibrary(static_cast<HMODULE>(m_library));
#endif
}

void cXessVulkanSupport::SetUnavailable(const char *reason) {
  m_available = false;
  CopyUnavailableReason(m_unavailableReason, reason);
}

cXessVulkanSupport &XessVulkanSupportInstance() {
  static cXessVulkanSupport support;
  return support;
}

} // namespace hpl
