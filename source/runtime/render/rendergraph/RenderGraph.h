#ifndef MOER_ENGINE_RENDER_GRAPH
#define MOER_ENGINE_RENDER_GRAPH

#include "RenderGraphPass.h"
#include "misc/STL.h"
#include "RenderAPI.h"
#include "string/String.h"

#include <cassert>
#include <cstdint>
#include <type_traits>
#include <typeindex>
#include <utility>

namespace Moer {

inline void RGAppendDecimal(String& target, uint64_t value) {
    String::value_type digits[20]{};
    uint32_t count = 0;
    do {
        digits[count++] = static_cast<String::value_type>('0' + (value % 10));
        value /= 10;
    } while (value != 0);

    while (count > 0) {
        target.push_back(digits[--count]);
    }
}

struct RGCompiledHazardEdge {
    uint32_t src_pass{0};
    uint32_t dst_pass{0};
    RenderGraphHandle resource{};
    ERGResourceKind resource_kind{ERGResourceKind::Texture};
};

struct RGCompiledPlan {
    Moer::Array<RGCompiledHazardEdge> hazard_edges{};
};

class RENDER_API RenderGraph {
public:
    RenderGraph() = default;
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    ~RenderGraph();

    template<typename T, typename... Args>
        requires std::is_constructible_v<T, Args...>
    T* Alloc(Args&&... args) {
        assert(m_phase == Phase::Setup && "RenderGraph parameters must be allocated during setup");
        auto* value = MoerNew(T)(std::forward<Args>(args)...);
        m_allocations.push_back(RGAllocation{
            .ptr = value,
            .destroy = [](void* ptr) { MoerDelete(static_cast<T*>(ptr)); },
            .type = std::type_index(typeid(T)),
            .size = static_cast<uint32_t>(sizeof(T))
        });
        return value;
    }

    RenderGraphHandle CreateTexture(StringView name, const RGTextureDesc& desc);
    RenderGraphHandle CreateBuffer(StringView name, const RGBufferDesc& desc);
    RenderGraphHandle ImportTexture(
        StringView             name,
        Render::TextureRef     texture,
        Render::EQueueType     owner_queue = Render::EQueueType::Graphics
    );
    RenderGraphHandle ImportBuffer(
        StringView            name,
        Render::BufferRef     buffer,
        Render::EQueueType    owner_queue = Render::EQueueType::Graphics
    );
    void ExportTexture(RenderGraphHandle handle, Render::ETextureState final_state, Render::EQueueType owner_queue);
    void ExportBuffer(RenderGraphHandle handle, Render::EBufferState final_state, Render::EQueueType owner_queue);

    using SetupExecute = std::function<void(RGSetupContext& setup)>;
    void AddSetupPass(StringView name, SetupExecute&& setup);

    template<typename T, typename Execute>
    void AddPass(T* parameters, ERGPassFlags flags, Execute&& execute) {
        String unnamed_name(MOER_TEXT("UnnamedPass_"));
        RGAppendDecimal(unnamed_name, static_cast<uint64_t>(m_passes.size()));
        AddPass(
            unnamed_name,
            parameters,
            flags,
            std::forward<Execute>(execute)
        );
    }

    template<typename T, typename Execute>
    void AddPass(StringView name, T* parameters, ERGPassFlags flags, Execute&& execute) {
        assert(parameters && "RenderGraph pass parameters must be graph-owned");
        using ExecuteType = std::remove_cvref_t<Execute>;
        constexpr bool is_parallel_execute = std::is_invocable_v<ExecuteType&, RHICommandList&, RGContext>;
        constexpr bool is_serial_execute   = std::is_invocable_v<ExecuteType&, RGContext>;
        static_assert(
            is_parallel_execute != is_serial_execute,
            "AddPass lambda must be either [](RHICommandList&, RGContext) for parallel work or [](RGContext) for serial work"
        );
        if constexpr (is_parallel_execute) {
            assert(RGPassHasQueue(flags) && "Parallel RenderGraph passes require one queue flag");
        } else {
            assert(flags == ERGPassFlags::None && "Serial RenderGraph passes must not declare queue flags");
        }

        auto collect_access = [](const void* raw_parameters, RGParameterAccessCollector& collector) {
            const auto& typed_parameters = *static_cast<const T*>(raw_parameters);
            if constexpr (requires(const T& value, RGParameterAccessCollector& access_collector) {
                              value.DeclareRGAccess(access_collector);
                          }) {
                typed_parameters.DeclareRGAccess(collector);
            }
        };

        if constexpr (is_parallel_execute) {
            RGPass::ParallelExecute parallel_execute = std::forward<Execute>(execute);
            AddPassInternal(
                String(name),
                parameters,
                std::type_index(typeid(T)),
                static_cast<uint32_t>(sizeof(T)),
                flags,
                ERGPassExecutionMode::Parallel,
                std::move(collect_access),
                std::move(parallel_execute),
                {}
            );
        } else {
            RGPass::SerialExecute serial_execute = std::forward<Execute>(execute);
            AddPassInternal(
                String(name),
                parameters,
                std::type_index(typeid(T)),
                static_cast<uint32_t>(sizeof(T)),
                flags,
                ERGPassExecutionMode::Serial,
                std::move(collect_access),
                {},
                std::move(serial_execute)
            );
        }
    }

    void Dispatch(RHICommandList* cmd_list = nullptr);
    void Reset();

    const RGCompiledPlan& GetCompiledPlan() const { return m_compiled_plan; }
    const Moer::Array<RGPass>& GetPasses() const { return m_passes; }
    const Moer::Array<RGResource>& GetResources() const { return m_resources; }

private:
    enum class Phase : uint8_t {
        Setup,
        Compiled,
        Dispatched
    };

    struct RGAllocation {
        void* ptr{nullptr};
        void (*destroy)(void*){nullptr};
        std::type_index type{typeid(void)};
        uint32_t size{0};
    };

    uint32_t AddPassInternal(
        String name,
        void* parameters,
        std::type_index type,
        uint32_t size,
        ERGPassFlags flags,
        ERGPassExecutionMode execution_mode,
        RGPass::CollectAccess&& collect_access,
        RGPass::ParallelExecute&& parallel_execute,
        RGPass::SerialExecute&& serial_execute
    );
    void AddTextureAccess(uint32_t pass_index, const RGTextureAccess& access);
    void AddBufferAccess(uint32_t pass_index, const RGBufferAccess& access);
    void RunSetupPasses();
    void CollectPassAccesses();
    void Compile();
    void EmitFinalTrackedStates(RHICommandList& cmd_list) const;
    RGResource& CheckedResource(RenderGraphHandle handle);
    const RGResource& CheckedResource(RenderGraphHandle handle) const;
    RenderGraphHandle AddResource(RGResource&& resource);
    void ValidateSetup() const;
    void BuildHazards();

    Phase           m_phase{Phase::Setup};
    bool            m_setup_executed{false};
    Moer::Array<RGResource>    m_resources{};
    Moer::Array<RGSetupPass>   m_setup_passes{};
    Moer::Array<RGPass>        m_passes{};
    Moer::Array<RGAllocation>  m_allocations{};
    RGCompiledPlan             m_compiled_plan{};
};

} // namespace Moer

#endif // !MOER_ENGINE_RENDER_GRAPH
