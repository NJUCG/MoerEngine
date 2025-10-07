#pragma once

/**
 * 用于Vulkan Cuda Interop
 * 
 * Ref: https://github.com/NVIDIA/cuda-samples/blob/master/Samples/5_Domain_Specific/vulkanImageCUDA/vulkanImageCUDA.cu
 */

#ifdef _WIN64

// Add windows.h to the include path firstly as dependency for other Windows headers
#include <windows.h>
// Add other Windows headers
#include <VersionHelpers.h>
#include <aclapi.h>
#include <dxgi1_2.h>

class WindowsSecurityAttributes {
protected:
    SECURITY_ATTRIBUTES  m_winSecurityAttributes;
    PSECURITY_DESCRIPTOR m_winPSecurityDescriptor;

public:
    WindowsSecurityAttributes();
    SECURITY_ATTRIBUTES* operator&();
    ~WindowsSecurityAttributes();
};

#endif