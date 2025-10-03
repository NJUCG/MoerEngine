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
    uint32_t             width{0}, height{0}, layers{1}, mips{1}, channal{4}, data_size{0};
    EPixelFormat         format{PF_UNDEFINED};
    Moer::Array<uint8_t> data;
    Array<uint32_t>      mip_offsets = {0};
    Array<Extent3D>      mip_extents;

    OutputStream& operator<<(OutputStream& _stream) const {
        _stream << width << height << layers << mips << channal << data_size << format << data << mip_offsets
                << mip_extents;
        return _stream;
    }

    InputStream& operator>>(InputStream& _stream) {
        _stream >> width >> height >> layers >> mips >> channal >> data_size >> format >> data >>
            mip_offsets >> mip_extents;
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
    // Path
    std::filesystem::path m_path;

    // Data obtained from Part 1 Nodes Prev:
    Moer::Array<Render::InstanceData> m_instance_infos{}; // gpu data, instance_info alse for cpu
    Moer::Array<MeshInstance>         m_mesh_instances{}; // loaded data

    Moer::Array<SharedPtr<Moer::MeshInfo>> m_mesh_infos{};      // loaded data
    Moer::Array<SharedPtr<MeshGeometry>>   m_mesh_geometries{}; // loaded data

    // Material
    Moer::UnorderedMap<std::string, TextureData>            m_textures{};
    Moer::UnorderedMap<std::string, Moer::uint32>           m_material_instance_indexes{};
    Array<Moer::MaterialInstanceRef>                        m_material_instances{};
    Moer::UnorderedMap<std::string, Moer::MaterialRef>      m_materials{};
    Moer::UnorderedMap<std::string, MatInstanceTextureInfo> m_mat_instance_textures{};

    // Data obtained from Part 2 Nodes Post:
    Moer::Array<SharedPtr<MeshBuffers>> m_mesh_buffers{}; // loaded data

    // Data obtained from Part 3 Camera&Light:
    Moer::Array<CameraRef>         m_cameras{};
    Moer::Array<LightComponentRef> m_lights{};
};
static_assert(is_ptr_t_v<SharedPtr<Moer::MeshInfo>>);
} // namespace Moer