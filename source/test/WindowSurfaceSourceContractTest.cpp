#include "rhi/RHIResource.h"
#include "rhi/RHIWindowSurface.h"

#include <cstdint>
#include <cstdlib>
#include <memory>

using namespace Moer::Render;

namespace {

void Require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

class FakeWindowSurfaceSource final : public WindowSurfaceSource {
public:
    WindowSurfaceIdentity identity{};
    WindowNativeHandle    native_window{};

    [[nodiscard]] WindowSurfaceIdentity GetIdentity() const noexcept override {
        return identity;
    }

    [[nodiscard]] WindowNativeHandle GetNativeWindow() const noexcept override {
        return native_window;
    }
};

std::shared_ptr<FakeWindowSurfaceSource>
MakeSource(uintptr_t window_system_handle, uintptr_t platform_window_handle, uint64_t generation) {
    auto source      = std::make_shared<FakeWindowSurfaceSource>();
    source->identity = {
        .window_system          = EWindowSystemType::GLFW,
        .window_system_handle   = window_system_handle,
        .platform_window_handle = platform_window_handle,
        .generation             = generation,
    };
    source->native_window = {
        .window_system          = EWindowSystemType::GLFW,
        .window_system_handle   = window_system_handle,
        .platform_window_handle = platform_window_handle,
    };
    return source;
}

} // namespace

int main() {
    const SwapchainSurfaceInfo invalid{};
    Require(!invalid.IsValid());
    Require(ClassifySwapchainSurfaceTransition({}, false, invalid) == ESwapchainSurfaceTransition::Invalid);

    auto                 source_a = MakeSource(0x1000u, 0x2000u, 1u);
    SwapchainSurfaceInfo surface_a{source_a};
    Require(surface_a.IsValid());
    Require(
        ClassifySwapchainSurfaceTransition({}, false, surface_a) == ESwapchainSurfaceTransition::Initialize
    );
    const SwapchainSurfaceInfo platform_handle_optional{MakeSource(0x1002u, 0u, 1u)};
    Require(platform_handle_optional.IsValid());
    Require(platform_handle_optional.source->GetNativeWindow().IsValid());
    Require(platform_handle_optional.source->GetNativeWindow().platform_window_handle == 0);
    Require(!WindowNativeHandle{}.IsValid());
    Require(!WindowNativeHandle{.window_system = EWindowSystemType::GLFW}.IsValid());
    auto inconsistent_source = MakeSource(0x1003u, 0x2003u, 1u);
    inconsistent_source->native_window.window_system_handle = 0x1004u;
    Require(!SwapchainSurfaceInfo{inconsistent_source}.IsValid());

    auto                 source_same_identity = MakeSource(0x1000u, 0x2000u, 1u);
    SwapchainSurfaceInfo surface_same_identity{source_same_identity};
    Require(source_a.get() != source_same_identity.get());
    Require(surface_a.HasSameIdentity(surface_same_identity));
    Require(
        ClassifySwapchainSurfaceTransition(surface_a, true, surface_same_identity) ==
        ESwapchainSurfaceTransition::Reuse
    );

    const SwapchainSurfaceInfo changed_window{MakeSource(0x1001u, 0x2000u, 1u)};
    const SwapchainSurfaceInfo changed_platform{MakeSource(0x1000u, 0x2001u, 1u)};
    const SwapchainSurfaceInfo changed_generation{MakeSource(0x1000u, 0x2000u, 2u)};
    Require(!surface_a.HasSameIdentity(changed_window));
    Require(!surface_a.HasSameIdentity(changed_platform));
    Require(!surface_a.HasSameIdentity(changed_generation));
    Require(
        ClassifySwapchainSurfaceTransition(surface_a, true, changed_window) ==
        ESwapchainSurfaceTransition::Replace
    );
    Require(
        ClassifySwapchainSurfaceTransition(surface_a, true, changed_platform) ==
        ESwapchainSurfaceTransition::Replace
    );
    Require(
        ClassifySwapchainSurfaceTransition(surface_a, true, changed_generation) ==
        ESwapchainSurfaceTransition::Replace
    );

    const WindowNativeHandle native_window = source_a->GetNativeWindow();
    Require(native_window.IsValid());
    Require(native_window.window_system == EWindowSystemType::GLFW);
    Require(native_window.window_system_handle == 0x1000u);
    Require(native_window.platform_window_handle == 0x2000u);

    std::weak_ptr<const WindowSurfaceSource> weak_source;
    SwapchainCreateInfo                      copied_info;
    {
        auto lifetime_source = MakeSource(0x7000u, 0x8000u, 1u);
        weak_source          = lifetime_source;
        SwapchainCreateInfo original{
            .surface = SwapchainSurfaceInfo{lifetime_source},
            .size    = {1280, 720},
        };
        copied_info = original;
        lifetime_source.reset();
        Require(!weak_source.expired());
    }
    Require(!weak_source.expired());
    copied_info.surface = {};
    Require(weak_source.expired());

    return 0;
}
