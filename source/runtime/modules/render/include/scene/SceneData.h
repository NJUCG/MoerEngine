#pragma once
#include "CameraManager.h"
#include "TransformManager.h"
#include "scene/Material.h"
#include "scene/Scene.h"
#include "scene/light/LightComponent.h"
#include "serialize/Serializer.h"
#include "shaderheaders/shared/Geometry.h"

#include <filesystem>
namespace Moer {
    struct MatInstanceTextureInfo {
        Moer::Array<std::pair<std::string, std::string>> textures;
    };

    struct TextureData {
        uint32_t              width{0}, height{0}, layers{1}, mips{1}, channal{4}, data_size{0};
        EPixelFormat          format{PF_UNDEFINED};
        Moer::Array<uint8_t>  data;
        std::vector<uint32_t> mip_offsets = {0};
        std::vector<Extent3D> mip_extents;

        OutputStream& operator<<(OutputStream& _stream) const {
            _stream << width << height << layers << mips << channal << data_size << format << data << mip_offsets << mip_extents;
            return _stream;
        }

        InputStream& operator>>(InputStream& _stream) {
            _stream >> width >> height >> layers >> mips >> channal >> data_size >> format >> data >> mip_offsets >> mip_extents;
            return _stream;
        }
    };

    struct PrimInfo {
        uint        mesh_id;
        std::string material_id;
        Transform   transform;

        OutputStream& operator<<(OutputStream& _stream) const {
            _stream << mesh_id << material_id << transform;
            return _stream;
        }

        InputStream& operator>>(InputStream& _stream) {
            _stream >> mesh_id >> material_id >> transform;
            return _stream;
        }
    };

    struct SceneData {
        Moer::Array<float> m_vertex_data{};

        Moer::Array<float>              m_position_data{};
        Moer::Array<float>              m_uv_data{};
        Moer::Array<float>              m_normal_data{};
        Moer::Array<uint32_t>           m_index_data{};
        Moer::Array<MeshletDesc>        m_meshlet_descs{};
        Moer::Array<Moer::MeshletBound> m_meshlet_bounds{};

        Moer::Array<PrimInfo>         m_prim_infos{};
        Moer::Array<InstanceMeshInfo> m_instance_mesh_info{};
        Moer::Array<uint32_t>         m_instance_id;

        //raytracing
        Moer::Array<RTVertex>    rt_vertices{};
        Moer::Array<RTInstance>  rt_instances{};
        Moer::Array<RTMeshInfo>  rt_mesh_infos{};
        Moer::Array<RTPrimitvie> rt_prims{};
        Moer::Array<uint3>       rt_indices{};

        //SOA layouts
        //loaded data
        Moer::Array<SharedPtr<Moer::MeshInfo>> m_mesh_infos{};
        Moer::Array<SharedPtr<MeshBuffers>>    m_mesh_buffers{};
        Moer::Array<SharedPtr<MeshGeometry>>   m_mesh_geometries{};
        Moer::Array<MeshInstance>              m_mesh_instances{};

        //gpu data, instance_info alse for cpu
        Moer::Array<Render::InstanceData> instance_infos{};
        Moer::Array<Render::GeometryData> geom_infos{};

        Moer::Array<CameraRef>         m_cameras{};
        Moer::Array<LightComponentRef> m_lights{};

        //material
        Moer::UnorderedMap<std::string, TextureData> m_textures{};

        Array<Moer::MaterialInstanceRef> m_material_instances{};

        Moer::UnorderedMap<std::string, MatInstanceTextureInfo> m_mat_instance_textures{};
        Moer::UnorderedMap<std::string, Moer::uint32>           m_material_instance_indexes;
        Moer::UnorderedMap<std::string, Moer::MaterialRef>      m_materials;

        std::filesystem::path m_path;
        size_t                m_scene_key{0};
        uint32_t              m_vertex_stride{0};
        uint32_t              m_index_stride{0};
    };
    static_assert(is_ptr_t_v<SharedPtr<Moer::MeshInfo>>);
}// namespace Moer