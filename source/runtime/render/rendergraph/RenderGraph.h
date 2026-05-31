#ifndef MOER_ENGINE_RENDER_GRAPH
#define MOER_ENGINE_RENDER_GRAPH

#include "RenderAPI.h"
#include "RenderGraphPass.h"
#include "RenderGraphResourcePool.h"
#include "misc/STL.h"
#include "string/Format.h"
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
    RGResource*        resource{nullptr};
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
    bool               async_record{true};
    bool               signal_sync_point{false};
    Moer::Array<uint32_t> wait_sync_point_batches{};
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

class RenderGraph;

class RENDER_API RGEventScope {
public:
    RGEventScope(RenderGraph& graph, String name);
    ~RGEventScope();

    RGEventScope(const RGEventScope&)            = delete;
    RGEventScope& operator=(const RGEventScope&) = delete;

private:
    RenderGraph* m_graph{nullptr};
};

inline String RGFormatEventScopeName(StringView name) {
    return String(name);
}

template<typename... Args>
String RGFormatEventScopeName(FormatString<Args...> format, Args&&... args) {
    return Printf(format, std::forward<Args>(args)...);
}

class RENDER_API RGTransientResourceAllocator {
public:
    RGTransientResourceAllocator(PooledTexturePool& texture_pool, PooledBufferPool& buffer_pool);

    static RGTransientResourceAllocator& Global();

    void Allocate(RenderGraph& graph);
    void Release(RenderGraph& graph);

private:
    PooledTexturePool& m_texture_pool;
    PooledBufferPool&  m_buffer_pool;
};

class RENDER_API RenderGraph {
public:
    RenderGraph();
    explicit RenderGraph(StringView name);
    RenderGraph(const RenderGraph&)            = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    ~RenderGraph();

    const String& Name() const {
        return m_name;
    }

    void PushEventScope(String name);
    void PopEventScope();

    template<typename T, typename... Args>
        requires std::is_constructible_v<T, Args...>
    T* Alloc(Args&&... args) {
        assert(!m_compiled && "RenderGraph parameters must be allocated before compile");
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

    RGTexture* CreateTexture(StringView name, const RGTextureDesc& desc);
    RGBuffer*  CreateBuffer(StringView name, const RGBufferDesc& desc);
    RGTexture* RegisterTexture(StringView name, PooledTextureRef texture, Render::EQueueType owner_queue);
    RGBuffer*  RegisterBuffer(StringView name, PooledBufferRef buffer, Render::EQueueType owner_queue);
    RGTexture* ImportTexture(StringView name, Render::TextureRef texture, Render::EQueueType owner_queue);
    RGBuffer*  ImportBuffer(StringView name, Render::BufferRef buffer, Render::EQueueType owner_queue);
    void       ExportTexture(RGTexture* texture, Render::ETextureState final_state, Render::EQueueType owner_queue);
    void       ExportBuffer(RGBuffer* buffer, Render::EBufferState final_state, Render::EQueueType owner_queue);

    using SetupExecute = std::function<void(RGSetupContext& setup)>;
    void AddSetupPass(StringView name, SetupExecute&& setup);
    using SetupCommandExecute = std::function<void(RHICommandList& cmd_list, RGSetupContext& setup)>;
    void AddSetupPass(StringView name, Render::EQueueType queue, SetupCommandExecute&& setup);

    template<typename T, typename Execute>
    void AddPass(T* parameters, ERGPassFlags flags, Execute&& execute) {
        String unnamed_name(MOER_TEXT("UnnamedPass_"));
        RGAppendDecimal(unnamed_name, static_cast<uint64_t>(m_passes.size()));
        AddPass(unnamed_name, parameters, flags, std::forward<Execute>(execute));
    }

    template<typename T, typename Execute>
    void AddPass(StringView name, T* parameters, ERGPassFlags flags, Execute&& execute) {
        assert(!m_compiled && "RenderGraph passes must be added before compile");
        ValidateBuildThread();
        assert(parameters && "RenderGraph pass parameters must be graph-owned");
        using ExecuteType                  = std::remove_cvref_t<Execute>;
        constexpr bool is_record_execute   = std::is_invocable_v<ExecuteType&, RHICommandList&, RGContext>;
        constexpr bool is_main_execute     = std::is_invocable_v<ExecuteType&, RGContext>;
        static_assert(
            is_record_execute != is_main_execute,
            "AddPass lambda must be either [](RHICommandList&, RGContext) for RHI recording or [](RGContext) for main-thread execution"
        );
        constexpr bool main_thread_execute = is_main_execute && !is_record_execute;
        assert(
            (main_thread_execute ? RGPassHasValidQueueFlags(flags) : RGPassHasQueue(flags)) &&
            "RenderGraph RHI recording passes require one queue flag; main-thread passes may use ERGPassFlags::None"
        );
        const bool serial = RGPassIsSerial(flags);
        assert(!(main_thread_execute && serial) && "Main-thread RG passes do not use ERGPassFlags::Serial");

        Moer::Array<RGTextureAccess> texture_accesses{};
        Moer::Array<RGBufferAccess>  buffer_accesses{};
        if constexpr (RGParameterAccessProvider<T>) {
            assert(!main_thread_execute && "Only RHI recording RG passes may declare RG resource access");
            if (!main_thread_execute) {
                RGParameterAccessCollector collector{RGPassQueue(flags)};
                parameters->DeclareRGAccess(collector);
                texture_accesses = collector.Textures();
                buffer_accesses  = collector.Buffers();
            }
        }

        RGPass::Execute pass_execute = [execute = std::forward<Execute>(execute)](
                                            RHICommandList* cmd_list,
                                            RGContext       context
                                        ) mutable {
            if constexpr (is_record_execute) {
                assert(cmd_list != nullptr);
                execute(*cmd_list, context);
            } else {
                assert(cmd_list == nullptr);
                execute(context);
            }
        };

        AddPassInternal(
            String(name),
            parameters,
            std::type_index(typeid(T)),
            static_cast<uint32_t>(sizeof(T)),
            flags,
            serial,
            main_thread_execute,
            std::move(texture_accesses),
            std::move(buffer_accesses),
            std::move(pass_execute)
        );
    }

    void Dispatch(RHICommandList* cmd_list = nullptr);
    void Dispatch(RGTransientResourceAllocator& allocator, RHICommandList* cmd_list = nullptr);
    void Reset();

    const RGCompiledPlan& GetCompiledPlan() const {
        return m_compiled_plan;
    }
    const Moer::Array<RGPass>& GetPasses() const {
        return m_passes;
    }
    const Moer::Array<RGResource*>& GetResources() const {
        return m_resources;
    }
    const RGTexture& GetTexture(const RGTexture* texture) const;
    const RGBuffer&  GetBuffer(const RGBuffer* buffer) const;

private:
    friend class RGTransientResourceAllocator;

    struct RGAllocation {
        void* ptr{nullptr};
        void (*destroy)(void*){nullptr};
        std::type_index type{typeid(void)};
        uint32_t        size{0};
    };

    struct RGExecutionState {
        ~RGExecutionState();

        Moer::UniquePtr<RenderGraph>          graph{};
        Moer::Array<Moer::UniquePtr<RGTexture>> textures{};
        Moer::Array<Moer::UniquePtr<RGBuffer>>  buffers{};
        Moer::Array<RGResource*>                resources{};
        Moer::Array<RGPass>                     passes{};
        Moer::Array<RGAllocation>               allocations{};
        RGCompiledPlan                          compiled_plan{};
    };

    uint32_t AddPassInternal(
        String                         name,
        void*                          parameters,
        std::type_index                type,
        uint32_t                       size,
        ERGPassFlags                   flags,
        bool                           serial,
        bool                           main_thread,
        Moer::Array<RGTextureAccess>&& texture_accesses,
        Moer::Array<RGBufferAccess>&&  buffer_accesses,
        RGPass::Execute&&              execute
    );
    GraphEventRef RunSetupPassesAsync();
    void       Compile(RGTransientResourceAllocator& allocator);
    void       DispatchBatched();
    void       EmitFinalTrackedStates(RHICommandList& cmd_list) const;
    void       EmitPassTransitions(RHICommandList& cmd_list, const RGPass& pass) const;
    SharedPtr<RGExecutionState> DetachExecutionState();
    RGTexture* AddTexture(Moer::UniquePtr<RGTexture> texture);
    RGBuffer*  AddBuffer(Moer::UniquePtr<RGBuffer> buffer);
    void       ValidateSetup() const;
    void       ValidateBuildThread() const;
    void       BuildResourceStateRanges();
    void       BuildCompileMetadata();
    void       BuildExecutionBatches();

    bool                          m_setup_executed{false};
    bool                          m_compiled{false};
    String                        m_name{MOER_TEXT("RenderGraph")};
    Moer::Array<String>           m_scope_stack{};
    Moer::Array<Moer::UniquePtr<RGTexture>> m_textures{};
    Moer::Array<Moer::UniquePtr<RGBuffer>>  m_buffers{};
    Moer::Array<RGResource*>       m_resources{};
    Moer::Array<RGSetupPass>       m_setup_passes{};
    Moer::Array<RGPass>            m_passes{};
    Moer::Array<RGAllocation>      m_allocations{};
    RGCompiledPlan                 m_compiled_plan{};
};

} // namespace Moer::Render

#define MOER_RG_EVENT_SCOPE_VAR_JOIN_IMPL(a, b) a##b
#define MOER_RG_EVENT_SCOPE_VAR_JOIN(a, b) MOER_RG_EVENT_SCOPE_VAR_JOIN_IMPL(a, b)
#define RG_EVENT_SCOPE(rendergraph, formatstring, ...)                                                \
    ::Moer::Render::RGEventScope MOER_RG_EVENT_SCOPE_VAR_JOIN(_moer_rg_event_scope_, __LINE__)(      \
        (rendergraph),                                                                                \
        ::Moer::Render::RGFormatEventScopeName((formatstring) __VA_OPT__(, ) __VA_ARGS__)             \
    )

#endif // !MOER_ENGINE_RENDER_GRAPH
