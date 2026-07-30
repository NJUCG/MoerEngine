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
    WindowSurfaceIdentity     identity{};
    WindowSurfaceCreateResult result{
        .status = EWindowSurfaceCreateStatus::Success,
    };
    uintptr_t native_surface{0xabcdu};

    mutable ERHIType    observed_rhi{ERHIType::Vulkan};
    mutable void*       observed_instance{nullptr};
    mutable const void* observed_allocator{nullptr};
    mutable void*       observed_output{nullptr};
    mutable uint32_t    create_calls{0};

    [[nodiscard]] WindowSurfaceIdentity GetIdentity() const noexcept override {
        return identity;
    }

    [[nodiscard]] WindowSurfaceCreateResult
    CreateSurface(ERHIType rhi_type, void* instance, const void* allocation_callbacks, void* surface)
        const noexcept override {
        observed_rhi       = rhi_type;
        observed_instance  = instance;
        observed_allocator = allocation_callbacks;
        observed_output    = surface;
        ++create_calls;
        if (result.Succeeded() && surface != nullptr) {
            *static_cast<uintptr_t*>(surface) = native_surface;
        }
        return result;
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

    auto*                           instance  = reinterpret_cast<void*>(0x3000u);
    auto*                           allocator = reinterpret_cast<const void*>(0x4000u);
    uintptr_t                       output    = 0;
    const WindowSurfaceCreateResult create_result =
        source_a->CreateSurface(ERHIType::D3D12, instance, allocator, &output);
    Require(create_result.Succeeded());
    Require(output == source_a->native_surface);
    Require(source_a->create_calls == 1);
    Require(source_a->observed_rhi == ERHIType::D3D12);
    Require(source_a->observed_instance == instance);
    Require(source_a->observed_allocator == allocator);
    Require(source_a->observed_output == &output);

    auto failing_source    = MakeSource(0x5000u, 0x6000u, 1u);
    failing_source->result = {
        .status            = EWindowSurfaceCreateStatus::NativeFailure,
        .native_error_code = -7,
    };
    output = 0;
    const WindowSurfaceCreateResult failure =
        failing_source->CreateSurface(ERHIType::Vulkan, instance, allocator, &output);
    Require(!failure.Succeeded());
    Require(failure.native_error_code == -7);
    Require(output == 0);

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
