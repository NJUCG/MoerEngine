#ifndef MOER_ENGINE_RENDER_GRAPH
#define MOER_ENGINE_RENDER_GRAPH

#include "RenderAPI.h"
#include "RenderGraphPass.h"
#include "RenderGraphResourcePool.h"
#include "misc/STL.h"
#include "string/String.h"

#include <cassert>
#include <cstdint>
#include <type_traits>
#include <typeindex>
#include <utility>

namespace Moer::Render {

inline void RGAppendDecimal(String& target, uint64_t value) {
    String::value_type digits[20]{};
    uint32_t           count = 0;
    do {
        digits[count++] = static_cast<String::value_type>('0' + (value % 10));
        value /= 10;
    } while (value != 0);

    while (count > 0) {
        target.push_back(digits[--count]);
    }
}

struct RGCompiledHazardEdge {
    uint32_t           src_pass{0};
    uint32_t           dst_pass{0};
    RenderGraphHandle  resource{};
    ERGResourceKind    resource_kind{ERGResourceKind::Texture};
    uint8_t            flags{0};
    Render::EQueueType src_queue{Render::EQueueType::Ignore};
    Render::EQueueType dst_queue{Render::EQueueType::Ignore};
};

struct RGCompiledExecutionBatch {
    Render::EQueueType queue{Render::EQueueType::Ignore};
    uint32_t           first_pass{0};
    uint32_t           pass_count{0};
    uint32_t           workload{0};
};

enum class ERGCompiledHazardFlag : uint8_t {
    AccessConflict = 1 << 0,
    OwnerTransfer  = 1 << 1
};

constexpr uint8_t RGCompiledHazardFlagMask(ERGCompiledHazardFlag flag) {
    return static_cast<uint8_t>(flag);
}

constexpr bool RGCompiledHazardHasFlag(const RGCompiledHazardEdge& edge, ERGCompiledHazardFlag flag) {
    return (edge.flags & RGCompiledHazardFlagMask(flag)) != 0;
}

struct RGCompiledPlan {
    Moer::Array<RGCompiledHazardEdge>     hazard_edges{};
    Moer::Array<RGCompiledExecutionBatch> execution_batches{};
};

class RENDER_API RenderGraph {
public:
    RenderGraph();
    RenderGraph(PooledTexturePool& texture_pool, PooledBufferPool& buffer_pool);
    RenderGraph(const RenderGraph&)            = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    ~RenderGraph();

    template<typename T, typename... Args>
        requires std::is_constructible_v<T, Args...>
    T* Alloc(Args&&... args) {
        assert(m_phase == ERGPhase::Setup && "RenderGraph parameters must be allocated during setup");
        auto* value = MoerNew(T)(std::forward<Args>(args)...);
        m_allocations.push_back(
            RGAllocation{
                .ptr = value,
                .destroy =
                    [](void* ptr) {
                        MoerDelete(static_cast<T*>(ptr));
                    },
                .type = std::type_index(typeid(T)),
                .size = static_cast<uint32_t>(sizeof(T))
            }
        );
        return value;
    }

    RenderGraphHandle CreateTexture(StringView name, const RGTextureDesc& desc);
    RenderGraphHandle CreateBuffer(StringView name, const RGBufferDesc& desc);
    RenderGraphHandle
    RegisterTexture(StringView name, PooledTextureRef texture, Render::EQueueType owner_queue);
    RenderGraphHandle RegisterBuffer(StringView name, PooledBufferRef buffer, Render::EQueueType owner_queue);
    RenderGraphHandle
    ImportTexture(StringView name, Render::TextureRef texture, Render::EQueueType owner_queue);
    RenderGraphHandle ImportBuffer(StringView name, Render::BufferRef buffer, Render::EQueueType owner_queue);
    void              ExportTexture(
        RenderGraphHandle     handle,
        Render::ETextureState final_state,
        Render::EQueueType    owner_queue
    );
    void
    ExportBuffer(RenderGraphHandle handle, Render::EBufferState final_state, Render::EQueueType owner_queue);

    using SetupExecute = std::function<void(RGSetupContext& setup)>;
    void AddSetupPass(StringView name, SetupExecute&& setup);

    template<typename T, typename Execute>
    void AddPass(T* parameters, ERGPassFlags flags, Execute&& execute) {
        String unnamed_name(MOER_TEXT("UnnamedPass_"));
        RGAppendDecimal(unnamed_name, static_cast<uint64_t>(m_passes.size()));
        AddPass(unnamed_name, parameters, flags, std::forward<Execute>(execute));
    }

    template<typename T, typename Execute>
    void AddPass(StringView name, T* parameters, ERGPassFlags flags, Execute&& execute) {
        assert(parameters && "RenderGraph pass parameters must be graph-owned");
        using ExecuteType                  = std::remove_cvref_t<Execute>;
        constexpr bool is_parallel_execute = std::is_invocable_v<ExecuteType&, RHICommandList&, RGContext>;
        constexpr bool is_serial_execute   = std::is_invocable_v<ExecuteType&, RGContext>;
        static_assert(
            is_parallel_execute != is_serial_execute,
            "AddPass lambda must be either [](RHICommandList&, RGContext) for parallel work or [](RGContext) "
            "for serial work"
        );
        if constexpr (is_parallel_execute) {
            assert(RGPassHasQueue(flags) && "Parallel RenderGraph passes require one queue flag");
        } else {
            assert(flags == ERGPassFlags::None && "Serial RenderGraph passes must not declare queue flags");
        }

        Moer::Array<RGTextureAccess> texture_accesses{};
        Moer::Array<RGBufferAccess>  buffer_accesses{};
        if constexpr (is_parallel_execute && RGParameterAccessProvider<T>) {
            RGParameterAccessCollector collector{RGPassQueue(flags)};
            parameters->DeclareRGAccess(collector);
            texture_accesses = collector.Textures();
            buffer_accesses  = collector.Buffers();
        }

        if constexpr (is_parallel_execute) {
            RGPass::ParallelExecute parallel_execute = std::forward<Execute>(execute);
            AddPassInternal(
                String(name),
                parameters,
                std::type_index(typeid(T)),
                static_cast<uint32_t>(sizeof(T)),
                flags,
                ERGPassExecutionMode::Parallel,
                std::move(texture_accesses),
                std::move(buffer_accesses),
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
                {},
                {},
                {},
                std::move(serial_execute)
            );
        }
    }

    void Dispatch(RHICommandList* cmd_list = nullptr);
    void Reset();

    const RGCompiledPlan& GetCompiledPlan() const {
        return m_compiled_plan;
    }
    const Moer::Array<RGPass>& GetPasses() const {
        return m_passes;
    }
    const Moer::Array<RGResource>& GetResources() const {
        return m_resources;
    }
    const RGTexture& GetTexture(RenderGraphHandle handle) const;
    const RGBuffer&  GetBuffer(RenderGraphHandle handle) const;

private:
    enum class ERGPhase : uint8_t {
        Setup,
        Compiled,
        Dispatched
    };

    struct RGAllocation {
        void* ptr{nullptr};
        void (*destroy)(void*){nullptr};
        std::type_index type{typeid(void)};
        uint32_t        size{0};
    };

    uint32_t AddPassInternal(
        String                    name,
        void*                     parameters,
        std::type_index           type,
        uint32_t                  size,
        ERGPassFlags              flags,
        ERGPassExecutionMode      execution_mode,
        Moer::Array<RGTextureAccess>&& texture_accesses,
        Moer::Array<RGBufferAccess>&&  buffer_accesses,
        RGPass::ParallelExecute&& parallel_execute,
        RGPass::SerialExecute&&   serial_execute
    );
    void              RunSetupPasses();
    void              Compile();
    void              AllocateTransientResources();
    void              ReleaseTransientResources();
    void              DispatchBatched();
    void              EmitFinalTrackedStates(RHICommandList& cmd_list) const;
    RGResource&       CheckedResource(RenderGraphHandle handle);
    const RGResource& CheckedResource(RenderGraphHandle handle) const;
    RenderGraphHandle AddResource(RGResource&& resource);
    void              ValidateSetup() const;
    void              BuildCompileMetadata();

    ERGPhase                  m_phase{ERGPhase::Setup};
    bool                      m_setup_executed{false};
    Moer::Array<RGResource>   m_resources{};
    Moer::Array<RGSetupPass>  m_setup_passes{};
    Moer::Array<RGPass>       m_passes{};
    Moer::Array<RGAllocation> m_allocations{};
    RGCompiledPlan            m_compiled_plan{};
    PooledTexturePool*        m_texture_pool{nullptr};
    PooledBufferPool*         m_buffer_pool{nullptr};
};

} // namespace Moer::Render

#endif // !MOER_ENGINE_RENDER_GRAPH
