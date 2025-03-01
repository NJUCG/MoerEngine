#pragma once

#include <concepts>
#include <type_traits>

#include "RenderAPI.h"
#include "misc/STL.h"
#include "misc/CompileTimeString.h"
#include "resources/vertexfactory/VertexAttributes.h"
#include "resources/vertexfactory/VertexFactoryBuffers.h"
#include "rhi/RHIResource.h"
#include "serialize/Serializer.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"

namespace Moer::Render {

    /**
     * Simplified PSO Manager
     */
    struct GbufferPsoRecord {
        VertexAttributesBitmask vertex_attributes_bitmask;
        std::string_view        vertex_shader_path;
        std::string_view        pixel_shader_path;
        std::string_view        vertex_shader_entry = "main";
        std::string_view        pixel_shader_entry  = "main";
    };

    template<typename TPipeline>
        requires std::is_base_of_v<RasterPipeline, TPipeline>
    class SimplifiedGbufferPsoManager {
    public:
        SimplifiedGbufferPsoManager(ShaderManager& manager, Array<GbufferPsoRecord> pso_records)
            : m_shader_manager(manager), m_pso_records(std::move(pso_records)) {
            // build index
            for (uint i = 0; i < m_pso_records.size(); i++) {
                m_pso_records_map[m_pso_records[i].vertex_attributes_bitmask] = i;
            }
        }

        ~SimplifiedGbufferPsoManager() = default;

        TPipeline& Get(const VertexAttributesBitmask& bitmask) {
            if (!m_pso_map.contains(bitmask)) {
                m_pso_map[bitmask] = CreatePso(bitmask);
            }
            return m_pso_map[bitmask];
        }

    private:
        TPipeline CreatePso(const VertexAttributesBitmask& bitmask) {
            LOG_INFO("Creating PSO for vertex attributes bitmask: {}", bitmask);

            VertexStream vertex_stream;

            const auto& attrs = VertexAttributesTool::GetArrayFromBitmask(bitmask);
            for (const auto& attr : attrs) {
                const auto& pixel_format = VertexAttributesTool::GetPixelFormat(attr);
                vertex_stream.EmplacePerVertex({Moer::Render::VertexElement(pixel_format)});
            }

            GfxPsoCreateInfo pso_info(RHIRasterizeInfo::Preset(),
                                      vertex_stream,
                                      {
                                          RHIColorAttachmentInfo::Preset(PF_R32_UINT),          // vbuffer
                                          RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_UNORM),    // normal
                                          RHIColorAttachmentInfo::Preset(PF_R32G32_SFLOAT),     // uv
                                          RHIColorAttachmentInfo::Preset(PF_R32G32B32A32_SFLOAT)// position
                                      },
                                      RHIDepthStencilStateInfo::Preset<DepthStencil::DEPTH_WRITE_GREATER>(),// depth buf
                                      PF_D32_SFLOAT_S8_UINT);

            const auto& record = m_pso_records[m_pso_records_map[bitmask]];

            return m_shader_manager
                .Raster()
                .Vertex(record.vertex_shader_path)
                .Pixel(record.pixel_shader_path)
                .Build<TPipeline>(std::move(pso_info));
        }

    private:
        // 注意，PsoManager的生命周期不能超过ShaderManager，否则m_shader_manager会变成悬垂引用
        ShaderManager&                                   m_shader_manager;
        Array<GbufferPsoRecord>                          m_pso_records;
        UnorderedMap<VertexAttributesBitmask, uint>      m_pso_records_map;
        UnorderedMap<VertexAttributesBitmask, TPipeline> m_pso_map;
    };

}// namespace Moer