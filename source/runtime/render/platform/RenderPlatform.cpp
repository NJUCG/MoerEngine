#include "platform/RenderPlatform.h"
namespace Moer {
    const char* RHI_VULKAN_NAME{"Vulkan"};
    const char* RHI_D3D12_NAME{"D3D12"};
    const char* RHI_OPENGL_NAME{"OpenGL"};
    const char* RHI_METAL_NAME{"Metal"};

    namespace RHI {
        void GenericRenderPlatformInfo::InitDefaultValues() {
        }

        void GenericRenderPlatformInfo::Initialize() {
        }

        void GenericRenderPlatformInfo::ParseValuesFromConfiguration(GenericRenderPlatformInfo& target_platform_info) {
        }
    }// namespace RHI
}// namespace Moer
