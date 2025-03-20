#include "shader/GeometryPassPsoManager.h"

#include "misc/MMemory.h"

#include <vector>

namespace Moer::Render {

    // MARK: PImpl Funcs

    struct GeometryPassPsoManager::Impl {

    public:
        Impl() : m_b_initialized(false), m_shader_manager(ShaderManager::Get()) {}
        Impl(const Impl&)            = delete;
        Impl& operator=(const Impl&) = delete;

        ~Impl() {}

        void BuildIndex() {
            for (uint i = 0; i < m_pso_records.size(); i++) {
                m_pso_records_map[m_pso_records[i].vertex_attributes_bitmask] = i;
            }
        }

        bool Initialize(const Array<GeometryPassPsoRecord>& pso_records) {
            if (m_b_initialized) {
                return false;
            }
            m_b_initialized = true;

            m_pso_records = pso_records;
            BuildIndex();
            return true;
        }

        bool Initialize(Array<GeometryPassPsoRecord>&& pso_records) {
            if (m_b_initialized) {
                return false;
            }
            m_b_initialized = true;

            m_pso_records = std::move(pso_records);
            BuildIndex();
            return true;
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
                    .vertex_shader_path        = "raster/geometry_pass/GeometryPassCommonVertex.hlsl",
                    .pixel_shader_path         = "raster/geometry_pass/GeometryPassCommonPixel.hlsl",
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

        GeometryPassPipeline CreatePso(const VertexAttributesBitmask& bitmask) {
            LOG_INFO("GeometryPassPsoManager, creating PSO for vertex attributes bitmask: {}", bitmask);

            if (!m_pso_records_map.contains(bitmask)) {
                LOG_ERROR("GeometryPassPsoManager, vertex attribute bitmask {} is not supported!", bitmask);
                return GeometryPassPipeline();
            }

            VertexStream vertex_stream;

            const auto& attrs = VertexAttributesTool::GetArrayFromBitmask(bitmask);
            for (const auto& attr : attrs) {
                const auto& pixel_format = VertexAttributesTool::GetPixelFormat(attr);
                vertex_stream.EmplacePerVertex({Moer::Render::VertexElement(pixel_format)});
            }

            GfxPsoCreateInfo pso_info(RHIRasterizeInfo::Preset(),
                                      vertex_stream,
                                      {
                                          RHIColorAttachmentInfo::Preset(PF_R32_UINT),                // vbuffer
                                          RHIColorAttachmentInfo::Preset(PF_A2R10G10B10_UNORM_PACK32),// normal
                                          RHIColorAttachmentInfo::Preset(PF_A2R10G10B10_UNORM_PACK32),// tangent
                                          RHIColorAttachmentInfo::Preset(PF_R32G32_SFLOAT),           // uv
                                          RHIColorAttachmentInfo::Preset(PF_R32G32B32A32_SFLOAT)      // position
                                      },
                                      RHIDepthStencilStateInfo::Preset<DepthStencil::DEPTH_WRITE_GREATER>(),// depth buf
                                      PF_D32_SFLOAT_S8_UINT);
            // 注：此处 RHIColorAttachmentInfo 的顺序需要和 GeometryPass.h 中的 ColorAttachment 顺序一致

            const auto& record = m_pso_records[m_pso_records_map[bitmask]];

            return m_shader_manager
                .Raster()
                .Vertex(record.vertex_shader_path, record.vertex_shader_entry, record.vertex_shader_environment)
                .Pixel(record.pixel_shader_path, record.pixel_shader_entry, record.pixel_shader_environment)
                .Build<GeometryPassPipeline>(std::move(pso_info));
        }

        GeometryPassPipeline& GetPso(const VertexAttributesBitmask& bitmask) {
            // whether initialized
            if (!m_b_initialized) {
                // You can initialize this outside. `m_b_initialized` records the state.
                auto res = Initialize(std::move(GetDefaultInitializers()));
                assert(res);
            }

            // whether pso exists
            if (!m_pso_map.contains(bitmask)) {
                m_pso_map[bitmask] = CreatePso(bitmask);
            }

            // return pso
            return m_pso_map[bitmask];
        }

        UnorderedMap<VertexAttributesBitmask, GeometryPassPipeline>& GetPsoMap() {
            return m_pso_map;
        }

    private:
        // 你问我为什么PImpl也要private？优雅，戊戌多盐（误）

        // Origin Data
        bool                         m_b_initialized = false;
        ShaderManager&               m_shader_manager;
        Array<GeometryPassPsoRecord> m_pso_records;

        // Derived Data
        UnorderedMap<VertexAttributesBitmask, uint>                 m_pso_records_map;
        UnorderedMap<VertexAttributesBitmask, GeometryPassPipeline> m_pso_map;
    };

    // MARK: Origin Funcs

    // 为了在ShutDown时能访问instance，并且在尚未初始化时0开销ShutDown
    // 只允许Get()和ShutDown()访问这个变量
    static GeometryPassPsoManager* instance;

    GeometryPassPsoManager& GeometryPassPsoManager::Get() {
        if (instance == nullptr) {
            instance = MoerNew(GeometryPassPsoManager);
        }
        return *instance;
    }

    void GeometryPassPsoManager::ShutDown() {
        if (instance == nullptr) { return; }

        // 调用~GeometryPassPsoManager()，释放资源
        MoerDelete(instance);
    }

    GeometryPassPipeline& GeometryPassPsoManager::GetPso(const VertexAttributesBitmask& bitmask) {
        return m_impl->GetPso(bitmask);
    }

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

}// namespace Moer::Render