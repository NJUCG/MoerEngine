#include "SceneCache.h"

#include "config/ConfigManager.h"
#include "misc/STL.h"
#include "misc/Timer.h"
#include "resources/GpuScene.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "scene/EntityManager.h"
#include "scene/Material.h"
#include "scene/RenderableManager.h"
#include "scene/Scene.h"
#include "scene/SceneData.h"
#include "scene/TransformManager.h"
#include "scene/light/LightComponent.h"
#include "scene/light/LightComponentManager.h"
#include "taskgraph/GraphTask.h"

#include <filesystem>
#include <fstream>

#include <serialize/Serializer.h>
#include <span>
#include <sstream>
#include <string>
namespace Moer {

class MaterialSystem {};

// class SceneCache::InputStream {
// public:
//     InputStream(std::istream& _stream) : m_stream(_stream) {}

//     void Read(void* _data, size_t _len) {
//         m_stream.read(reinterpret_cast<char*>(_data), _len);
//     }

//     template<typename T>
//     void Read(T& _value) {
//         Read(&_value, sizeof(T));
//     }

//     void Read(std::string& _value) {
//         uint64_t len = Read<uint64_t>();
//         _value.resize(len);
//         Read(_value.data(), len);
//     }

//     void Read(std::filesystem::path& _path) {
//         std::string str;
//         Read(str);
//         _path = str;
//     }

//     template<typename T>
//     T Read() {
//         T value;
//         Read(value);
//         return value;
//     }

//     template<typename T>
//     void Read(std::vector<T>& _vec) {
//         uint64_t len = Read<uint64_t>();
//         _vec.resize(len);
//         if constexpr (std::is_trivial<T>::value && !std::is_same<T, bool>::value) {
//             Read(_vec.data(), len * sizeof(T));
//         } else {
//             for (auto& item : _vec) Read(item);
//         }
//     }

//     template<typename T>
//     InputStream& operator>>(Array<T>& _value) {
//         uint64 len = Read<uint64>();
//         _value.resize(len);
//         for (auto& item : _value) {
//             *this >> item;
//         }
//         return *this;
//     }

//     template<typename T>
//     void Read(std::optional<T>& _opt) {
//         bool has_value = Read<bool>();
//         if (has_value) {
//             T value;
//             Read(value);
//             _opt = value;
//         }
//     }

//     template<typename T>
//     struct HasInputStreamOverloadFunction {
//         template<typename U>
//         static auto           Test(U* _p) -> decltype(std::declval<U>().operator>>(std::declval<InputStream&>()), std::true_type{});
//         static auto           Test(...) -> std::false_type;
//         static constexpr bool value = decltype(Test(static_cast<T*>(nullptr)))::value;
//     };

//     template<typename T>
//     InputStream& operator>>(T& _value) {
//         if constexpr (HasInputStreamOverloadFunction<T>::value) {
//             _value.operator>>(*this);
//         } else {
//             Read(_value);
//         }
//         return *this;
//     }

//     InputStream& operator>>(std::filesystem::path& _path) {
//         Read(_path);
//         return *this;
//     }

// private:
//     std::istream& m_stream;
// };

// class SceneCache::OutputStream {
// public:
//     OutputStream(std::ostream& stream) : mStream(stream) {}

//     void write(const void* data, size_t len) {
//         mStream.write(reinterpret_cast<const char*>(data), len);
//     }

//     template<typename T>
//     void write(const T& value) {
//         write(&value, sizeof(T));
//     }

//     void write(const std::string& value) {
//         uint64_t len = value.size();
//         write(len);
//         write(value.data(), len);
//     }

//     void write(const std::filesystem::path& path) {
//         write(path.string());
//     }

//     template<typename T>
//     void write(const std::vector<T>& vec) {
//         uint64_t len = vec.size();
//         write(len);
//         if constexpr (std::is_trivial<T>::value && !std::is_same<T, bool>::value) {
//             write(vec.data(), len * sizeof(T));
//         } else {
//             for (const auto& item : vec) write(item);
//         }
//     }

//     template<typename T>
//     void write(const std::optional<T>& opt) {
//         bool hasValue = opt.has_value();
//         write(hasValue);
//         if (hasValue) write(opt.value());
//     }

// private:
//     std::ostream& mStream;
// };

// static constexpr std::string SCENE_CACHE_EXT_SPECIFY = ".MOERSCENE";

static std::filesystem::path RemapScenePath(const std::filesystem::path& _path) {
    long long time = std::filesystem::exists(_path) ?
                         std::filesystem::last_write_time(_path).time_since_epoch().count() :
                         0;
    auto      cache_path =
        (ConfigManager::GetInstance().GetCachePath() /
         std::format("{}_{}", _path.filename().generic_string(), uint64(time)));
    return cache_path.generic_string() + ".MOERSCENE";
}

void SceneCache::FromFile(const std::filesystem::path& _path, Scene* _scene) {
    using namespace Moer;
    Timer timer;
    timer.Start();
    std::ifstream fs(_path, std::ios::binary);

    InputStream stream(fs);

    SceneData scene_data;

    ReadSceneGeomInfo(stream, scene_data);
    ReadSceneTextures(stream, scene_data);
    ReadSceneMaterial(stream, scene_data);
    ReadSceneUtils(stream, scene_data);

    ConvertToScene(scene_data, _scene, false);
    timer.Stop();
    LOG_INFO("Load Scene Cache Time(ms): {}", timer.ElapsedMilliseconds());
}
bool SceneCache::HasValidCache(const std::filesystem::path& _path) {
    auto cache_path = RemapScenePath(_path);
    return std::filesystem::exists(cache_path);
}
void SceneCache::ToFile(const Scene& scene, const std::filesystem::path& path) {}
void SceneCache::ReadSceneTextures(FInputStream& stream, SceneData& sceneData) {
    size_t texture_count;
    stream >> texture_count;

    for (uint i = 0; i < texture_count; ++i) {
        std::string name;
        stream >> name;

        TextureData texture_data;
        // stream >> texture_data.width;
        // stream >> texture_data.height;
        // stream >> texture_data.layers;
        // stream >> texture_data.mips;
        // stream >> texture_data.channel;
        // stream >> texture_data.format;
        // stream >> texture_data.data_size;

        // texture_data.data.resize(texture_data.data_size);
        // texture_data.mip_offsets.resize(texture_data.mips);
        // texture_data.mip_extents.resize(texture_data.mips);
        // stream.Read(texture_data.data.data(), texture_data.data_size);
        // stream.Read(texture_data.mip_offsets.data(), texture_data.mips * sizeof(uint32_t) * texture_data.layers);
        // stream.Read(texture_data.mip_extents.data(), texture_data.mips * sizeof(Extent3D) * texture_data.layers);

        stream >> texture_data;
        sceneData.m_textures[name] = std::move(texture_data);
    }
    // stream >> sceneData.m_textures;
}
void SceneCache::ReadSceneUtils(InputStream& stream, SceneData& _scene_data) {
    // read cameras
    size_t camera_count;
    stream >> camera_count;
    _scene_data.m_cameras.reserve(camera_count);
    for (int i = 0; i < camera_count; ++i) {
        CameraRef camera = MoerNew(Camera);
        // stream.Read(camera, sizeof(Camera));
        stream >> *camera;
        _scene_data.m_cameras.push_back(camera);
    }

    // read lights
    size_t light_count;
    stream >> light_count;
    _scene_data.m_lights.reserve(light_count);
    for (int i = 0; i < light_count; ++i) {
        LightComponentRef light = LightComponent::ReadFromStream(stream);
        _scene_data.m_lights.push_back(light);
    }
}

void SceneCache::ReadSceneGeomInfo(FInputStream& _stream, SceneData& _scene_data) {
    _stream >> _scene_data.m_mesh_buffers >> _scene_data.m_mesh_instances >> _scene_data.m_mesh_geometries >>
        _scene_data.m_mesh_infos;

    //fill mesh infos
    for (auto& mesh_info : _scene_data.m_mesh_infos) {
        for (uint i = 0; i < mesh_info->geometries.size(); ++i) {
            // intialize mesh_info -> mesh_geo
            mesh_info->geometries[i] = _scene_data.m_mesh_geometries[mesh_info->geom_start_idx + i];
            // intialize mesh_geo -> mesh_buffers
            auto geo          = mesh_info->geometries[i];
            geo->mesh_buffers = _scene_data.m_mesh_buffers[geo->mesh_buffers_idx];
        }
    }
    // //fill mesh instances
    // for (auto& mesh_instance : _scene_data.m_mesh_instances) {
    //     mesh_instance.mesh_info = _scene_data.m_mesh_infos[mesh_instance.mesh_info_idx];
    // }

    _stream >> _scene_data.m_instance_infos;
}

void SceneCache::ReadSceneMaterial(InputStream& _stream, SceneData& _scene_data) {
    size_t material_count;
    _stream >> material_count;

    for (int i = 0; i < material_count; ++i) {
        std::string material_name;
        _stream >> material_name;

        // block
        BufferInterfaceBlock block;
        {
            _stream >> block.m_name;

            _stream >> block.m_size;
            _stream >> block.m_alignment;
            _stream >> block.m_target;
            _stream >> block.m_qualifiers;

            _stream >> block.m_field_info_list;

            size_t info_count;
            _stream >> info_count;
            for (int j = 0; j < info_count; ++j) {
                std::string name;
                _stream >> name;
                uint32_t offset;
                _stream >> offset;
                block.m_info_map[block.m_field_info_list[offset].name] = offset;
            }
        }
        // sampler
        TextureInterfaceBlock sampler;
        {
            _stream >> sampler.m_name;

            _stream >> sampler.m_sampler_info_list;

            size_t info_count;
            _stream >> info_count;
            for (int j = 0; j < info_count; ++j) {
                std::string name;
                _stream >> name;
                uint8_t offset;
                _stream >> offset;
                sampler.m_info_map[sampler.m_sampler_info_list[offset].name] = offset;
            }
        }

        MaterialRef material = MoerNew(Material);
        material->SetBufferInterfaceBlock(block);
        material->SetSamplerInterfaceBlock(sampler);
        material->SetName(material_name);

        _scene_data.m_materials[material_name] = material;
    }

    size_t material_instance_count;
    _stream >> material_instance_count;
    for (int i = 0; i < material_instance_count; ++i) {
        std::string material_instance_name;
        _stream >> material_instance_name;
        uint inst_idx;
        _stream >> inst_idx;
        _scene_data.m_material_instance_indexes[material_instance_name] = inst_idx;
    }
    _scene_data.m_material_instances.resize(material_instance_count);

    for (int i = 0; i < material_instance_count; ++i) {
        std::string material_instance_name;
        _stream >> material_instance_name;
        std::string mat_name;
        _stream >> mat_name;
        auto   material_instance = _scene_data.m_materials[mat_name]->CreateInstance();
        size_t texture_param_count;
        _stream >> texture_param_count;
        for (int j = 0; j < texture_param_count; ++j) {
            std::string param_name;
            _stream >> param_name;
            std::string texture_name;
            _stream >> texture_name;
            _scene_data.m_mat_instance_textures[material_instance_name].textures.emplace_back(
                param_name, texture_name
            );
        }

        auto unfirom_buffer_size = material_instance->GetUniformBuffer().GetSize();
        auto buffer_data         = Moer::Array<uint8_t>(unfirom_buffer_size);
        _stream >> buffer_data;
        material_instance->SetUnifomBuffer(buffer_data.data(), unfirom_buffer_size);
        material_instance->SetName(material_instance_name);
        _scene_data.m_material_instances[_scene_data.m_material_instance_indexes[material_instance_name]] =
            material_instance;
    }
}

void SceneCache::WriteSceneGeomInfo(FOutputStream& _stream, const SceneData& _scene_data) {
    //cpu data
    _stream << _scene_data.m_mesh_buffers;
    _stream << _scene_data.m_mesh_instances;
    _stream << _scene_data.m_mesh_geometries;
    _stream << _scene_data.m_mesh_infos;

    //gpu data
    _stream << _scene_data.m_instance_infos;
}
void SceneCache::WriteSceneMaterial(FOutputStream& _stream, const SceneData& _scene_data) {
    _stream << _scene_data.m_materials.size();
    for (auto& material : _scene_data.m_materials) {
        _stream << material.first;
        auto& sampler_info = material.second->GetSamplerInterfaceBlock();
        auto& buffer_info  = material.second->GetBufferInterfaceBlock();

        {
            _stream << buffer_info.m_name;
            _stream << buffer_info.m_size;
            _stream << buffer_info.m_alignment;
            _stream << buffer_info.m_target;
            _stream << buffer_info.m_qualifiers;

            _stream << buffer_info.m_field_info_list;

            _stream << buffer_info.m_info_map.size();
            for (auto& info : buffer_info.m_info_map) {
                _stream << info.first;
                _stream << info.second;
            }
        }

        {
            _stream << sampler_info.m_name;
            _stream << sampler_info.m_sampler_info_list;
            _stream << sampler_info.m_info_map.size();
            for (auto& info : sampler_info.m_info_map) {
                _stream << info.first;
                _stream << info.second;
            }
        }
    }

    _stream << _scene_data.m_material_instances.size();
    for (auto [name, index] : _scene_data.m_material_instance_indexes) {
        _stream << name << index;
    }
    for (auto [name, index] : _scene_data.m_material_instance_indexes) {
        auto& material_instance = _scene_data.m_material_instances.at(index);
        _stream << name;
        _stream << material_instance->GetMaterial()->GetName();

        if (_scene_data.m_mat_instance_textures.contains(name)) {
            const MatInstanceTextureInfo& mat_instance_texture_info =
                _scene_data.m_mat_instance_textures.at(name);
            _stream << mat_instance_texture_info.textures.size();

            for (auto& texture : mat_instance_texture_info.textures) {
                _stream << texture.first;
                _stream << texture.second;
            }

        } else {
            // Because in ReadSceneMaterial(), we expect the texture_param_count to be a size_t
            // Here we need to manual cast 0 to size_t to ensure it is written in 8 bytes instead of 4
            // stream.write(static_cast<size_t>(0));
            _stream << 0ull;
        }
        _stream << std::span<byte>(
            (byte*)material_instance->GetUniformBuffer().GetData(),
            material_instance->GetUniformBuffer().GetSize()
        );
    }
}
void SceneCache::WriteSceneTextures(FOutputStream& _stream, const SceneData& _scene_data) {
    _stream << _scene_data.m_textures.size();

    for (auto& texture : _scene_data.m_textures) {
        _stream << texture.first;
        _stream << texture.second;
    }
}
void SceneCache::WriteSceneUtils(OutputStream& _stream, const SceneData& _scene_data) {
    // write cameras
    _stream << _scene_data.m_cameras.size();
    for (auto& camera : _scene_data.m_cameras) {
        _stream << *camera;
    }

    // write lights
    _stream << _scene_data.m_lights.size();
    for (auto& light : _scene_data.m_lights) {
        light->WriteToStream(_stream);
    }
}

SceneData SceneCache::ConvertToSceneData(const Scene& _scene) {
    //May be not necessary
    SceneData scene_data;
    return scene_data;
}

size_t HashSceneData(const SceneData& _scene_data) {
    size_t hash = 0;
    return hash;
}

void SceneCache::Cache(const SceneData& _scene_data, size_t _key) {
    std::filesystem::path path = RemapScenePath(_scene_data.m_path);
    if (!std::filesystem::exists(path.parent_path())) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream fs(path, std::ios::binary);
    OutputStream  stream(fs);
    WriteSceneGeomInfo(stream, _scene_data);
    WriteSceneTextures(stream, _scene_data);
    WriteSceneMaterial(stream, _scene_data);
    WriteSceneUtils(stream, _scene_data);
}

void SceneCache::ConvertToScene(SceneData& _scene_data, Scene* _scene, bool _need_cache) {
    using namespace Moer::Render;
    using namespace Moer;

    auto& device = Render::RenderDevice::Get();
    /******************* create instance entities */
    for (auto& instance : _scene_data.m_mesh_instances) {
        auto entity = EntityManager::Get().Create();
        _scene->AddEntity(entity);
        RenderableManager::Get().CreateMeshInstance(entity);
        RenderableManager::Get().SetMeshInfo(entity, _scene_data.m_mesh_infos[instance.mesh_info_idx]);
        RenderableManager::Get().SetInstanceID(entity, instance.instance_id);
        Array<MaterialInstanceRef> material_instances;
        SharedPtr<MeshInfo>        mesh_info = _scene_data.m_mesh_infos[instance.mesh_info_idx];

        material_instances.reserve(mesh_info->geometries.size());
        for (const auto& geo : mesh_info->geometries) {
            material_instances.emplace_back(_scene_data.m_material_instances[geo->material_id]);
        }
        RenderableManager::Get().SetMaterialInstances(entity, std::move(material_instances));
        // RenderableManager::Get().SetRTMeshInfo(entity, _scene_data.rt_mesh_infos[instance.instance_id]);
        TransformManager::Get().Create(entity);

        TransformManager::Get().Set(entity, _scene_data.m_instance_infos[instance.instance_id].model2world);
    }

    assert(_scene_data.m_mesh_instances.size() != 0 && "Mesh instances should not be empty");

    /******************* create camera entities */
    for (auto& camera : _scene_data.m_cameras) {
        auto entity = EntityManager::Get().Create();
        CameraManager::Get().Put(entity, camera);
        _scene->AddCamera(entity);
    }

    /******************* create light entities */
    for (auto& light : _scene_data.m_lights) {
        auto entity = EntityManager::Get().Create();
        LightComponentManager::Get().Put(entity, light);
        _scene->AddLight(entity);
    }

    /******************* create material textures, build to dx/vulkan images, store (name, tex-handle) to material instance buffer */
    Moer::UnorderedMap<std::string, Render::TextureRef> textures;
    Moer::Array<TextureBuilder>                         texture_builders;
    texture_builders.reserve(_scene_data.m_textures.size());
    for (auto& texture : _scene_data.m_textures) {
        auto& builder = texture_builders.emplace_back();
        builder.Data(texture.second.data.data(), texture.second.data.size());
        builder.Width(texture.second.width);
        builder.Height(texture.second.height);
        builder.Format(texture.second.format);
        builder.MipAndLayers(texture.second.mips, texture.second.layers);
        builder.Name(texture.first);
    }
    textures = TextureBuilder::BuildTexturesInBatch(texture_builders);
    Render::Sampler                         sampler(SF_LINEAR, SAM_REPEAT);
    UnorderedMap<std::string, SceneTexture> scene_textures;
    //  scene_data.m_textures = GpuSceneBufferBuilder::
    for (auto& material_instance : _scene_data.m_material_instances) {

        if (!_scene_data.m_mat_instance_textures.contains(material_instance->GetName())) {
            continue;
        }
        for (auto& texture : _scene_data.m_mat_instance_textures[material_instance->GetName()].textures) {
            uint32_t handle = _scene->GetBindlessArray()->AllocateTexture(
                textures[texture.second]->GetView(0, textures[texture.second]->GetNumMips()), sampler
            );
            material_instance->SetParameter(texture.first, handle);
            scene_textures[texture.second] = {textures[texture.second], handle};
        }
    }
    _scene->RegisterMaterialTextures(scene_textures);

    Moer::Array<byte> material_data(_scene_data.m_material_instances.size() * Material::MaterialBytesNum);
    for (auto [name, index] : _scene_data.m_material_instance_indexes) {
        auto& material = _scene_data.m_material_instances[index];
        memcpy(
            material_data.data() + index * Material::MaterialBytesNum,
            material->GetUniformBuffer().GetData(),
            material->GetUniformBuffer().GetSize()
        );
    }

    /******************* copyqueue, copy mesh buffers, texture images, material buffers, light buffers to GPU */
    Render::CommandList cmd_list{};

    auto  bindless_array = _scene->GetBindlessArray();
    auto& copy_queue     = device.GetCopyQueue();

    for (auto& buf : _scene_data.m_mesh_buffers) {

        // vertex buffer

        uint vertex_size = 0;

        // clang-format off
            size_t position_buffer_size =
                buf->vertex_factory_buffers.HasAttribute(EVertexAttributes::VA_POSITION)
                ? buf->vertex_factory_buffers.GetBufferByteSize(EVertexAttributes::VA_POSITION)
                : 0;

            size_t normal_buffer_size =
                buf->vertex_factory_buffers.HasAttribute(EVertexAttributes::VA_NORMAL)
                ? buf->vertex_factory_buffers.GetBufferByteSize(EVertexAttributes::VA_NORMAL)
                : 0;

            size_t tangent_buffer_size =
                buf->vertex_factory_buffers.HasAttribute(EVertexAttributes::VA_TANGENT)
                ? buf->vertex_factory_buffers.GetBufferByteSize(EVertexAttributes::VA_TANGENT)
                : 0;

            size_t texcoord0_buffer_size =
                buf->vertex_factory_buffers.HasAttribute(EVertexAttributes::VA_TEXCOORD0)
                ? buf->vertex_factory_buffers.GetBufferByteSize(EVertexAttributes::VA_TEXCOORD0)
                : 0;

            size_t texcoord1_buffer_size =
                buf->vertex_factory_buffers.HasAttribute(EVertexAttributes::VA_TEXCOORD1)
                ? buf->vertex_factory_buffers.GetBufferByteSize(EVertexAttributes::VA_TEXCOORD1)
                : 0;
        // clang-format on

        vertex_size += position_buffer_size;
        vertex_size += normal_buffer_size;
        vertex_size += tangent_buffer_size;
        vertex_size += texcoord0_buffer_size;
        vertex_size += texcoord1_buffer_size;

        buf->vertex_buffer = device.CreateBuffer<byte>(
            "Scene::soa_vertex_buffer",
            vertex_size,
            EBufferUsageFlags::VERTEX_BUFFER | EBufferUsageFlags::ACCELERATION_STRUCTURE |
                EBufferUsageFlags::UNORDERED_ACCESS
        );
        if (position_buffer_size > 0) {
            auto* position_buffer_ptr =
                buf->vertex_factory_buffers.GetBufferData(EVertexAttributes::VA_POSITION);
            cmd_list.CopyFrom(
                std::span<byte>((byte*)position_buffer_ptr, position_buffer_size),
                buf->vertex_buffer->GetView(0, buf->GetAttributeRange(EVertexAttributes::VA_POSITION).size),
                "CopyFrom MeshBuffers position_buffer"
            );
        }
        if (normal_buffer_size > 0) {
            auto* normal_buffer_ptr = buf->vertex_factory_buffers.GetBufferData(EVertexAttributes::VA_NORMAL);
            cmd_list.CopyFrom(
                std::span<byte>((byte*)normal_buffer_ptr, normal_buffer_size),
                buf->vertex_buffer->GetView(
                    buf->GetAttributeRange(EVertexAttributes::VA_NORMAL).offset,
                    buf->GetAttributeRange(EVertexAttributes::VA_NORMAL).size
                ),
                "CopyFrom MeshBuffers normal_buffer"
            );
        }
        if (tangent_buffer_size > 0) {
            auto* tangent_buffer_ptr =
                buf->vertex_factory_buffers.GetBufferData(EVertexAttributes::VA_TANGENT);
            cmd_list.CopyFrom(
                std::span<byte>((byte*)tangent_buffer_ptr, tangent_buffer_size),
                buf->vertex_buffer->GetView(
                    buf->GetAttributeRange(EVertexAttributes::VA_TANGENT).offset,
                    buf->GetAttributeRange(EVertexAttributes::VA_TANGENT).size
                ),
                "CopyFrom MeshBuffers tangent_buffer"
            );
        }
        if (texcoord0_buffer_size > 0) {
            auto* texcoord0_buffer_ptr =
                buf->vertex_factory_buffers.GetBufferData(EVertexAttributes::VA_TEXCOORD0);
            cmd_list.CopyFrom(
                std::span<byte>((byte*)texcoord0_buffer_ptr, texcoord0_buffer_size),
                buf->vertex_buffer->GetView(
                    buf->GetAttributeRange(EVertexAttributes::VA_TEXCOORD0).offset,
                    buf->GetAttributeRange(EVertexAttributes::VA_TEXCOORD0).size
                ),
                "CopyFrom MeshBuffers texcoord0_buffer"
            );
        }
        if (texcoord1_buffer_size > 0) {
            auto* texcoord1_buffer_ptr =
                buf->vertex_factory_buffers.GetBufferData(EVertexAttributes::VA_TEXCOORD1);
            cmd_list.CopyFrom(
                std::span<byte>((byte*)texcoord1_buffer_ptr, texcoord1_buffer_size),
                buf->vertex_buffer->GetView(
                    buf->GetAttributeRange(EVertexAttributes::VA_TEXCOORD1).offset,
                    buf->GetAttributeRange(EVertexAttributes::VA_TEXCOORD1).size
                ),
                "CopyFrom MeshBuffers texcoord1_buffer"
            );
        }

        // index buffer

        buf->index_buffer = device.CreateBuffer<uint32_t>(
            "Scene::index_buffer",
            buf->indices.size(),
            EBufferUsageFlags::INDEX_BUFFER | EBufferUsageFlags::ACCELERATION_STRUCTURE |
                EBufferUsageFlags::UNORDERED_ACCESS
        );

        cmd_list.CopyFrom(
            std::span<byte>((byte*)buf->indices.data(), buf->indices.size() * sizeof(uint32_t)),
            buf->index_buffer->GetView(),
            "CopyFrom MeshBuffers index_buffer"
        );

        // bindless handle
        buf->idx_bdls_handle = bindless_array->AllocateBuffer(buf->index_buffer->GetView());
        buf->vtx_bdls_handle = bindless_array->AllocateBuffer(buf->vertex_buffer->GetView());

        /******************* import mesh buffers from cpu-io to the copy queue */
        _scene->EmplaceIOImportedBuffer(buf->vertex_buffer);
        _scene->EmplaceIOImportedBuffer(buf->index_buffer);
    }

    _scene->UpdateGpuData();

    // auto evt = copy_queue.Execute(cmd_list.Submit());
    // copy_queue.Sync(evt.timeline);

    auto instance_data_buffer = device.CreateBuffer<byte>(
        "Scene::InstanceDataBuffer",
        _scene->GetInstanceDatas().size_bytes(),
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto material_buffer = device.CreateBuffer<byte>(
        "Scene::MaterialInstanceBuffer",
        _scene_data.m_material_instances.size() * Material::MaterialBytesNum,
        EBufferUsageFlags::UNORDERED_ACCESS
    );

    auto geometry_data_buffer = device.CreateBuffer<byte>(
        "Scene::GeometryDataBuffer",
        _scene->GetGeometryDatas().size_bytes(),
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto geometry_instance_buffer = device.CreateBuffer<byte>(
        "Scene::GeometryInstanceBuffer",
        _scene->GetGeometryInstances().size_bytes(),
        EBufferUsageFlags::UNORDERED_ACCESS
    );

    Array<LightComponentData> lights(_scene_data.m_lights.size());
    for (uint i = 0; i < _scene_data.m_lights.size(); ++i) {
        lights[i] = _scene_data.m_lights[i]->ToData();
    }

    BufferRef light_buffer = device.CreateBuffer<byte>(
        "Scene::LightBuffer", lights.size() * sizeof(LightComponentData), EBufferUsageFlags::UNORDERED_ACCESS
    );

    cmd_list.CopyFrom(material_data, material_buffer->GetView(), "CopyFrom material_buffer");

    cmd_list.CopyFrom(
        std::span<byte>((byte*)lights.data(), lights.size() * sizeof(LightComponentData)),
        light_buffer->GetView(),
        "CopyFrom light_buffer"
    );

    cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)_scene->GetInstanceDatas().data(),
            _scene->GetInstanceDatas().size() * sizeof(Render::InstanceData)
        ),
        instance_data_buffer->GetView(),
        "CopyFrom instance_data_buffer"
    );

    cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)_scene->GetGeometryDatas().data(),
            _scene->GetGeometryDatas().size() * sizeof(Render::GeometryData)
        ),
        geometry_data_buffer->GetView(),
        "CopyFrom geometry_data_buffer"
    );

    cmd_list.CopyFrom(
        std::span<byte>(
            (byte*)_scene->GetGeometryInstances().data(),
            _scene->GetGeometryInstances().size() * sizeof(Render::GeometryInstance)
        ),
        geometry_instance_buffer->GetView(),
        "CopyFrom geometry_instance_buffer"
    );

    auto evt = copy_queue.Execute(cmd_list.Submit());
    copy_queue.Sync(evt.timeline);

    _scene->SetBuffer(EGpuSceneResource::InstanceInfo, instance_data_buffer);
    _scene->SetBuffer(EGpuSceneResource::MaterialInfo, material_buffer);
    _scene->SetBuffer(EGpuSceneResource::GeometryInfo, geometry_data_buffer);
    _scene->SetBuffer(EGpuSceneResource::GeometryInstance, geometry_instance_buffer);
    _scene->SetBuffer(EGpuSceneResource::LightInfo, light_buffer);

    /******************* import material buffers, light buffers, geometry data buffers, instance data buffers from cpu-io queue to the gpu copy queue */
    _scene->EmplaceIOImportedBuffer(light_buffer);
    _scene->EmplaceIOImportedBuffer(material_buffer);
    _scene->EmplaceIOImportedBuffer(geometry_data_buffer);
    _scene->EmplaceIOImportedBuffer(instance_data_buffer);
    _scene->EmplaceIOImportedBuffer(geometry_instance_buffer);

    /******************* export textures and buffers from the gpu copy queue to the gpu graphics queue */
    Array<Render::ExportTexture> export_textures;
    export_textures.reserve(textures.size());

    for (auto& texture : textures) {
        export_textures.push_back({texture.second->GetView(), ETextureState::SAMPLE});
    }

    Array<Render::ExportBuffer> export_buffers;
    export_buffers.reserve(_scene->GetIOPendingBuffers().size());

    for (auto& buffer : _scene->GetIOPendingBuffers()) {
        export_buffers.push_back({buffer->GetView(), EBufferState::UNORDERED_ACCESS});
    }

    cmd_list.ExportResourcesToQueue(
        EQueueType::Graphics, std::move(export_textures), std::move(export_buffers)
    );

    auto copy_handle = copy_queue.GetFenceHandle();

    //wait all texture export done
    evt = copy_queue.Execute(cmd_list.Submit().Wait(copy_handle, copy_handle->GetValue()));

    copy_queue.Sync(evt.timeline);

    Array<ImportTexture> sampled_textures;
    sampled_textures.reserve(textures.size());

    for (auto& [name, tex] : textures) {
        sampled_textures.emplace_back(
            ImportTexture(tex->GetView(0, tex->GetNumMips()), ETextureState::SAMPLE)
        );
    }

    Array<ImportBuffer> io_buffers;
    io_buffers.reserve(_scene->GetIOPendingBuffers().size());

    for (auto& buffer : _scene->GetIOPendingBuffers()) {
        io_buffers.emplace_back(ImportBuffer(buffer->GetView()));
    }

    cmd_list.ImportResourcesFromQueue(EQueueType::Copy, std::move(sampled_textures), std::move(io_buffers));

    auto& gfx_queue = device.GetCommandQueue(EQueueType::Graphics);
    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

    // BuildSceneRaytracing(scene_data,scene.get());

    // Cache the scene data
    // Tip: I move this function to the end of the function, to avoid storing wrong cache to disk.
    //      Wrong cache may be caused by out-date loader code.
    //      When loading wrong cache, the engine will crash with no information.
    size_t hash    = HashSceneData(_scene_data);
    bool   updated = true;
    if (updated && _need_cache) {
        Cache(_scene_data, hash);
    }
}
void SceneCache::LoadSceneFromCacheAsync(const std::filesystem::path& path, Scene* scene) {
    LambdaTask::Dispatch([path, scene]() {
        AsyncSceneLoadInfoRef load_info = MoerNew(AsyncSceneLoadInfo)();
        load_info->b_valid              = true;
        load_info->progress.store(0);
        auto remapped_path = RemapScenePath(path);
        LOG_INFO("Scene Cache Path: {}", remapped_path.string());
        FromFile(remapped_path, scene);
        load_info->progress.store(1);
    });
}
void SceneCache::LoadSceneFromCache(const std::filesystem::path& path, Scene* scene) {
    AsyncSceneLoadInfoRef load_info = MoerNew(AsyncSceneLoadInfo)();
    load_info->b_valid              = true;
    load_info->progress.store(0);
    Scene::RegisterAsyncLoadInfo(load_info);
    auto remapped_path = RemapScenePath(path);
    LOG_INFO("Scene Cache Path: {}", remapped_path.string());
    FromFile(remapped_path, scene);
    load_info->progress.store(1);
}

} // namespace Moer