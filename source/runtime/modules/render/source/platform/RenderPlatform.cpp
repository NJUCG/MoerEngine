#include "platform/RenderPlatform.h"
using namespace Moer::RHI;
using string = std::string;
const string RHI_VULKAN_NAME("Vulkan");
const string RHI_D3D_NAME("D3D");
const string RHI_OPENGL_NAME("OpenGL");
const string RHI_METAL_NAME("Metal");

void GenericRenderPlatformInfo::InitDefaultValues() {
}

void GenericRenderPlatformInfo::Initialize() {
}

void GenericRenderPlatformInfo::ParseValuesFromConfiguration(const ConfigMap& _value_map, GenericRenderPlatformInfo& target_platform_info) {
}
