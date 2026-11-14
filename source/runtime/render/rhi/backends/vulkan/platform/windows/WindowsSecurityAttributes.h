#pragma once

/**
 * 用于Vulkan Cuda Interop
 * 
 * Ref: https://github.com/NVIDIA/cuda-samples/blob/master/Samples/5_Domain_Specific/vulkanImageCUDA/vulkanImageCUDA.cu
 * 
 * 25.10.15 Update: CudaPass中创建信号量时，是否添加 VkExportSemaphoreWin32HandleInfoKHR 貌似不影响结果。
 *                  我先删去CudaPass中引用这个类的代码；如果之后hw项目结束后（大概26年），这个片段仍未被使用，则可以删去这个类
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