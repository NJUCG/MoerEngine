#ifndef MOER_ENGINE_RENDER_GRAPH
#define MOER_ENGINE_RENDER_GRAPH

#include "RenderGraphPass.h"
#include "misc/STL.h"
#include "RenderAPI.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <utility>

namespace Moer {

struct RGFrameReceipt {
    RenderGraphHandle   resource{};
    Render::EQueueType  owner_queue{Render::EQueueType::Graphics};
    Render::ETextureState texture_state{Render::ETextureState::UNDEFINED};
    Render::EBufferState  buffer_state{Render::EBufferState::UNDEFINED};
    uint64_t            completion_value{0};
};

class RENDER_API RGFrameContext {
public:
    explicit RGFrameContext(uint64_t frame_sequence = 0) : m_frame_sequence(frame_sequence) {}

    uint64_t NextGraphSequence();
    uint64_t GetFrameSequence() const { return m_frame_sequence; }
    void     PublishReceipt(const RGFrameReceipt& receipt);
    const RGFrameReceipt* FindReceipt(RenderGraphHandle resource) const;
    void     Reset(uint64_t frame_sequence);

private:
    uint64_t m_frame_sequence{0};
    uint64_t m_next_graph_sequence{0};
    Moer::Array<RGFrameReceipt> m_receipts{};
};

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
    explicit RenderGraph(RGFrameContext& frame_context);
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

    RenderGraphHandle CreateTexture(std::string_view name, const RGTextureDesc& desc);
    RenderGraphHandle CreateBuffer(std::string_view name, const RGBufferDesc& desc);
    RenderGraphHandle ImportTexture(
        std::string_view       name,
        Render::TextureRef     texture,
        Render::ETextureState  initial_state,
        Render::EQueueType     owner_queue = Render::EQueueType::Graphics
    );
    RenderGraphHandle ImportBuffer(
        std::string_view      name,
        Render::BufferRef     buffer,
        Render::EBufferState  initial_state,
        Render::EQueueType    owner_queue = Render::EQueueType::Graphics
    );
    void ExportTexture(RenderGraphHandle handle, Render::ETextureState final_state, Render::EQueueType owner_queue);
    void ExportBuffer(RenderGraphHandle handle, Render::EBufferState final_state, Render::EQueueType owner_queue);

    using SetupExecute = std::function<void(RGSetupContext& setup)>;
    void AddSetupPass(std::string_view name, SetupExecute&& setup);

    template<typename T, typename Execute>
    void AddPass(T* parameters, ERGPassFlags flags, Execute&& execute) {
        AddPass(
            "UnnamedPass_" + std::to_string(m_passes.size()),
            parameters,
            flags,
            std::forward<Execute>(execute)
        );
    }

    template<typename T, typename Execute>
    void AddPass(std::string_view name, T* parameters, ERGPassFlags flags, Execute&& execute) {
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
                std::string(name),
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
                std::string(name),
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

    void Compile();
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
        std::string name,
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
    RGResource& CheckedResource(RenderGraphHandle handle);
    const RGResource& CheckedResource(RenderGraphHandle handle) const;
    RenderGraphHandle AddResource(RGResource&& resource);
    void ValidateSetup() const;
    void BuildHazards();

    RGFrameContext& m_frame_context;
    uint64_t        m_graph_sequence{0};
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
