#include "shader/GeometryPassPsoManager.h"

#include "misc/MMemory.h"
#include "shader/ShaderCommon.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace Moer::Render {
struct PsoMapKey {
    VertexAttributesBitmask bitmask;
    bool                    is_shadow_depth_pass;

    PsoMapKey(VertexAttributesBitmask bitmask, bool is_shadow_depth_pass) :
        bitmask(bitmask),
        is_shadow_depth_pass(is_shadow_depth_pass) {}

    bool operator==(const PsoMapKey& other) const {
        return bitmask == other.bitmask && is_shadow_depth_pass == other.is_shadow_depth_pass;
    }
};
} // namespace Moer::Render

namespace std {
template<>
struct hash<Moer::Render::PsoMapKey> {
    size_t operator()(const Moer::Render::PsoMapKey& key) const {
        return std::hash<Moer::VertexAttributesBitmask>()(key.bitmask) ^
               std::hash<bool>()(key.is_shadow_depth_pass);
    }
};
} // namespace std

namespace Moer::Render {

// MARK: PImpl Funcs

struct GeometryPassPsoManager::Impl {

public:
    Impl() : m_shader_manager(ShaderManager::Get()) {}
    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {}

    void Initialize(const Array<GeometryPassPsoRecord>& pso_records) {
        m_pso_records = pso_records;
        // build index
        for (uint i = 0; i < m_pso_records.size(); i++) {
            m_pso_records_map[m_pso_records[i].vertex_attributes_bitmask] = i;
        }
    }

    Array<GeometryPassPsoRecord> GetDefaultInitializers() {
        Array<GeometryPassPsoRecord> ans;

        // MARK: * Supported Bitmask
        // clang-format off
            Array<VertexAttributesBitmask> supported_bitmask = {
                VertexAttributesTool::GetBitmaskFromArray({
                    EVertexAttributes::VA_POSITION,
                    EVertexAttributes::VA_NORMAL
                }),
                VertexAttributesTool::GetBitmaskFromArray({
                    EVertexAttributes::VA_POSITION,
                    EVertexAttributes::VA_NORMAL,
                    EVertexAttributes::VA_TANGENT,
                    EVertexAttributes::VA_TEXCOORD0
                }),
                VertexAttributesTool::GetBitmaskFromArray({
                    EVertexAttributes::VA_POSITION,
                    EVertexAttributes::VA_NORMAL,
                    EVertexAttributes::VA_TANGENT,
                    EVertexAttributes::VA_TEXCOORD0,
                    EVertexAttributes::VA_TEXCOORD1
                }),
            };
        // clang-format on

        // MARK: * Generate Records
        for (const auto& bitmask : supported_bitmask) {
            ans.push_back({
                .vertex_attributes_bitmask = bitmask,
                .vertex_shader_path        = "pipelines/raster/deferred/GeometryPassCommonVertex.hlsl",
                .pixel_shader_path         = "pipelines/raster/deferred/GeometryPassCommonPixel.hlsl",
                .vertex_shader_entry       = "main",
                .pixel_shader_entry        = "main",
                .vertex_shader_environment = [&]() -> ShaderCompilerEnvironment {
                    ShaderCompilerEnvironment env;
                    if (VertexAttributesTool::HasAttribute(bitmask, EVertexAttributes::VA_TANGENT)) {
                        env.SetDefine("HAS_TANGENT", 1);
                    }
                    if (VertexAttributesTool::HasAttribute(bitmask, EVertexAttributes::VA_TEXCOORD0)) {
                        env.SetDefine("HAS_TEXCOORD0", 1);
                    }
                    if (VertexAttributesTool::HasAttribute(bitmask, EVertexAttributes::VA_TEXCOORD1)) {
                        env.SetDefine("HAS_TEXCOORD1", 1);
                    }
                    return env;
                }(),
                .pixel_shader_environment = {},
            });
        }

        return ans;
    }

    VertexStream GetVertexStream(const VertexAttributesBitmask& bitmask) {
        VertexStream vertex_stream;

        const auto& attrs = VertexAttributesTool::GetArrayFromBitmask(bitmask);
        for (const auto& attr : attrs) {
            const auto& pixel_format = VertexAttributesTool::GetPixelFormat(attr);
            vertex_stream.EmplacePerVertex({Moer::Render::VertexElement(pixel_format)});
        }

        return vertex_stream;
    }

    // // MARK: GeometryPass
    // GeometryPassPipeline CreateGeometryPso(const VertexAttributesBitmask& bitmask) {
    //     LOG_INFO("GeometryPassPsoManager, creating GeometryPass PSO for vertex attributes bitmask: {}", bitmask);

    //     if (!m_pso_records_map.contains(bitmask)) {
    //         LOG_ERROR("GeometryPassPsoManager, vertex attribute bitmask {} is not supported!", bitmask);
    //         return GeometryPassPipeline();
    //     }

    //     VertexStream vertex_stream = GetVertexStream(bitmask);

    //     GfxPsoCreateInfo pso_info(RHIRasterizeInfo::Preset(),
    //                               vertex_stream,
    //                               {
    //                                   RHIColorAttachmentInfo::Preset(PF_R32_UINT),                // vbuffer
    //                                   RHIColorAttachmentInfo::Preset(PF_A2R10G10B10_UNORM_PACK32),// normal
    //                                   RHIColorAttachmentInfo::Preset(PF_A2R10G10B10_UNORM_PACK32),// tangent
    //                                   RHIColorAttachmentInfo::Preset(PF_R32G32_SFLOAT),           // uv
    //                                   RHIColorAttachmentInfo::Preset(PF_R32G32B32A32_SFLOAT)      // position
    //                               },
    //                               RHIDepthStencilStateInfo::Preset<DepthStencil::DEPTH_WRITE_GREATER>(),// depth buf
    //                               PF_D32_SFLOAT_S8_UINT);
    //     // 注：此处 RHIColorAttachmentInfo 的顺序需要和 GeometryPass.h 中的 ColorAttachment 顺序一致

    //     const auto&   record = m_pso_records[m_pso_records_map[bitmask]];
    //     VertexFactory factory{bitmask};
    //     return m_shader_manager
    //         .Raster()
    //         .Vertex(record.vertex_shader_path, record.vertex_shader_entry, &factory)
    //         .Pixel(record.pixel_shader_path, record.pixel_shader_entry, &factory)
    //         .Build<GeometryPassPipeline>(std::move(pso_info));
    // }

    // ShadowDepthPassPipeline CreateShadowPso(const VertexAttributesBitmask& bitmask) {
    //     LOG_INFO("GeometryPassPsoManager, creating ShadowDepthPass PSO for vertex attributes bitmask: {}", bitmask);

    //     if (!m_pso_records_map.contains(bitmask)) {
    //         LOG_ERROR("GeometryPassPsoManager, vertex attribute bitmask {} is not supported!", bitmask);
    //         return ShadowDepthPassPipeline();
    //     }

    //     VertexStream vertex_stream = GetVertexStream(bitmask);

    //     GfxPsoCreateInfo pso_info(RHIRasterizeInfo::Preset(),
    //                               vertex_stream,
    //                               {},
    //                               RHIDepthStencilStateInfo::Preset<DepthStencil::DEPTH_WRITE_GREATER>(),// depth buf
    //                               PF_D32_SFLOAT_S8_UINT);

    //     auto record = m_pso_records[m_pso_records_map[bitmask]];

    //     record.vertex_shader_environment.SetDefine("SHADOW_DEPTH_PASS", 1);
    //     record.pixel_shader_environment.SetDefine("SHADOW_DEPTH_PASS", 1);

    //     // return m_shader_manager
    //     //     .Raster()
    //     //     .Vertex(record.vertex_shader_path, record.vertex_shader_entry, record.vertex_shader_environment)
    //     //     .Pixel(record.pixel_shader_path, record.pixel_shader_entry, record.pixel_shader_environment)
    //     //     .Build<ShadowDepthPassPipeline>(std::move(pso_info));
    //     assert(false);
    //     return ShadowDepthPassPipeline{};
    // }

    // GeometryPassPipeline& GetGeometryPso(const VertexAttributesBitmask& bitmask) {
    //     // whether pso exists
    //     if (!m_geometry_pso_map.contains(bitmask)) {
    //         m_geometry_pso_map[bitmask] = CreateGeometryPso(bitmask);
    //     }

    //     // return pso
    //     return m_geometry_pso_map[bitmask];
    // }

    // ShadowDepthPassPipeline& GetShadowPso(const VertexAttributesBitmask& bitmask) {
    //     // whether pso exists
    //     if (!m_shadow_pso_map.contains(bitmask)) {
    //         m_shadow_pso_map[bitmask] = CreateShadowPso(bitmask);
    //     }

    //     // return pso
    //     return m_shadow_pso_map[bitmask];
    // }

private:
    // 你问我为什么PImpl也要private？优雅，戊戌多盐（误）

    // Origin Data
    ShaderManager&               m_shader_manager;
    Array<GeometryPassPsoRecord> m_pso_records;

    // Derived Data
    UnorderedMap<VertexAttributesBitmask, uint> m_pso_records_map;
    // UnorderedMap<VertexAttributesBitmask, GeometryPassPipeline>    m_geometry_pso_map;
    // UnorderedMap<VertexAttributesBitmask, ShadowDepthPassPipeline> m_shadow_pso_map;
};

// MARK: Origin Funcs

// 为了在ShutDown时能访问instance，并且在尚未初始化时0开销ShutDown
// 只允许Get()和ShutDown()访问这个变量
static std::atomic<GeometryPassPsoManager*> instance_atomic;
static std::mutex                           instance_mutex;

GeometryPassPsoManager& GeometryPassPsoManager::Get() {
    GeometryPassPsoManager* instance = instance_atomic.load(std::memory_order_acquire);
    if (instance == nullptr) {
        std::lock_guard<std::mutex> lock(instance_mutex);

        instance = instance_atomic.load(std::memory_order_relaxed);
        if (instance == nullptr) {
            instance = MoerNew(GeometryPassPsoManager);
            instance->m_impl->Initialize(instance->m_impl->GetDefaultInitializers());
            instance_atomic.store(instance, std::memory_order_release);
        }
    }
    return *instance;
}

void GeometryPassPsoManager::ShutDown() {
    std::lock_guard<std::mutex> lock(instance_mutex);

    GeometryPassPsoManager* instance = instance_atomic.exchange(nullptr, std::memory_order_acquire);

    if (instance) {
        // 调用~GeometryPassPsoManager()，释放资源
        MoerDelete(instance);
    }
}

// GeometryPassPipeline& GeometryPassPsoManager::GetGeometryPso(const VertexAttributesBitmask& bitmask) {
//     return m_impl->GetGeometryPso(bitmask);
// }

// ShadowDepthPassPipeline& GeometryPassPsoManager::GetShadowPso(const VertexAttributesBitmask& bitmask) {
//     return m_impl->GetShadowPso(bitmask);
// }

GeometryPassPsoManager::GeometryPassPsoManager() {
    assert(m_impl == nullptr);
    m_impl = MoerNew(GeometryPassPsoManager::Impl);
}

GeometryPassPsoManager::~GeometryPassPsoManager() {
    if (m_impl) {
        MoerDelete(m_impl);
        m_impl = nullptr;
    }
}

} // namespace Moer::Render