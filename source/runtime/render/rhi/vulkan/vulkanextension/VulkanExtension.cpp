#include "VulkanExtension.h"
#include "VulkanExtensionRegistry.h"

namespace Moer::Render {

namespace {

template<typename Fn>
void ForEachExtensionDesc(EVulkanExtensionKind _kind, Fn&& _fn) {
    for (const auto& desc : GetVulkanExtensionDescs()) {
        if (desc.kind != _kind) {
            continue;
        }
        _fn(desc);
    }
}

std::shared_ptr<VulkanDeviceExtension> CreateDeviceExtension(const VulkanExtensionDesc& _desc) {
    if (_desc.factory) {
        return _desc.factory(_desc.optional);
    }
    return std::make_shared<VulkanDeviceExtension>(_desc.name, _desc.optional);
}

} // namespace

TExtensionArray VulkanInstanceExtension::GetMERequiredInstanceExtensions() {
    TExtensionArray extensions;
    ForEachExtensionDesc(EVulkanExtensionKind::Instance, [&](const VulkanExtensionDesc& _desc) {
        if (!_desc.optional) {
            extensions.emplace_back(_desc.name);
        }
    });
    return extensions;
}

TExtensionArray VulkanInstanceExtension::GetMEOptionalInstanceExtensions() {
    TExtensionArray extensions;
    ForEachExtensionDesc(
        EVulkanExtensionKind::Instance,
        [&](const VulkanExtensionDesc& _desc) {
            if (_desc.optional) {
                extensions.emplace_back(_desc.name);
            }
        }
    );
    return extensions;
}

TVulkanDeviceExtensionArray VulkanDeviceExtension::GetMERequiredDeviceExtensions() {
    TVulkanDeviceExtensionArray extensions;
    ForEachExtensionDesc(EVulkanExtensionKind::Device, [&](const VulkanExtensionDesc& _desc) {
        extensions.emplace_back(CreateDeviceExtension(_desc));
    });
    return extensions;
}

TVulkanDeviceExtensionArray
VulkanDeviceExtension::GetMEEnabledDeviceExtensions(const Set<std::string>& _gpu_extensions) {
    auto extensions = GetMERequiredDeviceExtensions();

    TVulkanDeviceExtensionArray extensions_enabled;

    for (const auto& ext : extensions) {
        if (_gpu_extensions.contains(ext->GetExtensionName().data())) {
            ext->Enable();
            extensions_enabled.emplace_back(ext);
        }
    }

    return extensions_enabled;
}

} // namespace Moer::Render
