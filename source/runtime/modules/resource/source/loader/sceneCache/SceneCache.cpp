#include "SceneCache.h"

#include "config/ConfigManager.h"
#include "misc/Timer.h"
#include "resources/GpuScene.h"
#include "rhi/RHI.h"
#include "scene/EntityManager.h"
#include "taskgraph/GraphTask.h"

#include <fstream>
#include <filesystem>
namespace Moer {

    class MaterialSystem {
    };

    class SceneCache::InputStream {
    public:
        InputStream(std::istream& stream) : mStream(stream) {}

        void read(void* data, size_t len) {
            mStream.read(reinterpret_cast<char*>(data), len);
        }

        template<typename T>
        void read(T& value) {
            read(&value, sizeof(T));
        }

        void read(std::string& value) {
            uint64_t len = read<uint64_t>();
            value.resize(len);
            read(value.data(), len);
        }

        void read(std::filesystem::path& path) {
            std::string str;
            read(str);
            path = str;
        }

        template<typename T>
        T read() {
            T value;
            read(value);
            return value;
        }

        template<typename T>
        void read(std::vector<T>& vec) {
            uint64_t len = read<uint64_t>();
            vec.resize(len);
            if constexpr (std::is_trivial<T>::value && !std::is_same<T, bool>::value) {
                read(vec.data(), len * sizeof(T));
            } else {
                for (auto& item : vec) read(item);
            }
        }

        template<typename T>
        void read(std::optional<T>& opt) {
            bool hasValue = read<bool>();
            if (hasValue) {
                T value;
                read(value);
                opt = value;
            }
        }

    private:
        std::istream& mStream;
    };

    class SceneCache::OutputStream {
    public:
        OutputStream(std::ostream& stream) : mStream(stream) {}

        void write(const void* data, size_t len) {
            mStream.write(reinterpret_cast<const char*>(data), len);
        }

        template<typename T>
        void write(const T& value) {
            write(&value, sizeof(T));
        }

        void write(const std::string& value) {
            uint64_t len = value.size();
            write(len);
            write(value.data(), len);
        }

        void write(const std::filesystem::path& path) {
            write(path.string());
        }

        template<typename T>
        void write(const std::vector<T>& vec) {
            uint64_t len = vec.size();
            write(len);
            if constexpr (std::is_trivial<T>::value && !std::is_same<T, bool>::value) {
                write(vec.data(), len * sizeof(T));
            } else {
                for (const auto& item : vec) write(item);
            }
        }

        template<typename T>
        void write(const std::optional<T>& opt) {
            bool hasValue = opt.has_value();
            write(hasValue);
            if (hasValue) write(opt.value());
        }

    private:
        std::ostream& mStream;
    };

    // static constexpr std::string SCENE_CACHE_EXT_SPECIFY = ".MOERSCENE";

    static std::filesystem::path RemapScenePath(const std::filesystem::path& path) {
        return path.string() + ".MOERSCENE";
    }

    UniquePtr<Scene> SceneCache::FromFile(const std::filesystem::path& path) {
        Moer::Timer timer;
        timer.Start();
        std::ifstream fs(path, std::ios::binary);
        InputStream   stream(fs);
        SceneData     sceneData;

        ReadSceneGeomInfo(stream, sceneData);
        ReadSceneTextures(stream, sceneData);
        ReadSceneMaterial(stream, sceneData);
        ReadSceneUtils(stream, sceneData);

        auto scene = ConvertToScene(sceneData, false);
        timer.Stop();
        LOG_INFO("Load Scene Cache Time(ms): {}", timer.ElapsedMilliseconds());
        return scene;
    }
    bool SceneCache::HasValidCache(const std::filesystem::path& path) {
        auto cache_path = RemapScenePath(path);
        return std::filesystem::exists(cache_path);
    }
    void SceneCache::ToFile(const Scene& scene, const std::filesystem::path& path) {
    }
    void SceneCache::ReadSceneTextures(InputStream& stream, SceneData& sceneData) {
        size_t texture_count;
        stream.read(texture_count);

        for (uint i = 0; i < texture_count; ++i) {
            std::string name;
            stream.read(name);

            TextureData texture_data;
            stream.read(texture_data.width);
            stream.read(texture_data.height);
            stream.read(texture_data.layers);
            stream.read(texture_data.mips);
            stream.read(texture_data.channal);
            stream.read(texture_data.format);
            stream.read(texture_data.data_size);

            texture_data.data.resize(texture_data.data_size);
            texture_data.mip_offsets.resize(texture_data.mips);
            texture_data.mip_extents.resize(texture_data.mips);
            stream.read(texture_data.data.data(), texture_data.data_size);
            stream.read(texture_data.mip_offsets.data(), texture_data.mips * sizeof(uint32_t) * texture_data.layers);
            stream.read(texture_data.mip_extents.data(), texture_data.mips * sizeof(Extent3D) * texture_data.layers);

            sceneData.m_textures[name] = std::move(texture_data);
        }
    }
    void SceneCache::ReadSceneUtils(InputStream& stream, SceneData& sceneData) {
        size_t camera_count;
        stream.read(camera_count);
        sceneData.m_cameras.reserve(camera_count);
        for (int i = 0; i < camera_count; ++i) {
            CameraRef camera = MoerNew(Camera);
            stream.read(camera, sizeof(Camera));
            sceneData.m_cameras.push_back(camera);
        }
    }

    void SceneCache::ReadSceneGeomInfo(InputStream& stream, SceneData& sceneData) {
        size_t vertex_count;
        stream.read(vertex_count);
        sceneData.m_vertex_data.resize(vertex_count);
        stream.read(sceneData.m_vertex_data.data(), vertex_count * sizeof(float));

        size_t index_count;
        stream.read(index_count);
        sceneData.m_index_data.resize(index_count);
        stream.read(sceneData.m_index_data.data(), index_count * sizeof(uint32_t));

        size_t meshlet_desc_count;
        stream.read(meshlet_desc_count);
        sceneData.m_meshlet_descs.resize(meshlet_desc_count);
        stream.read(sceneData.m_meshlet_descs.data(), meshlet_desc_count * sizeof(MeshletDesc));

        size_t meshlet_bound_count;
        stream.read(meshlet_bound_count);
        sceneData.m_meshlet_bounds.resize(meshlet_bound_count);
        stream.read(sceneData.m_meshlet_bounds.data(), meshlet_bound_count * sizeof(MeshletBound));

        size_t mesh_info_count;
        stream.read(mesh_info_count);
        sceneData.m_mesh_infos.resize(mesh_info_count);
        stream.read(sceneData.m_mesh_infos.data(), mesh_info_count * sizeof(MeshInfo));

        size_t prim_info_count;
        stream.read(prim_info_count);
        sceneData.m_prim_infos.resize(prim_info_count);
        for (int i = 0; i < prim_info_count; ++i) {
            stream.read(sceneData.m_prim_infos[i].mesh_id);
            stream.read(sceneData.m_prim_infos[i].material_id);
            stream.read(sceneData.m_prim_infos[i].transform);
        }

        size_t instance_data_count;
        stream.read(instance_data_count);
        sceneData.m_instance_data.resize(instance_data_count);
        stream.read(sceneData.m_instance_data.data(), instance_data_count * sizeof(InstanceData));

        size_t instance_mesh_info_count;
        stream.read(instance_mesh_info_count);
        sceneData.m_instance_mesh_info.resize(instance_mesh_info_count);
        stream.read(sceneData.m_instance_mesh_info.data(), instance_mesh_info_count * sizeof(InstanceMeshInfo));

        size_t instance_id_count;
        stream.read(instance_id_count);
        sceneData.m_instance_id.resize(instance_id_count);
        stream.read(sceneData.m_instance_id.data(), instance_id_count * sizeof(uint32_t));
    }

    void SceneCache::ReadSceneMaterial(InputStream& stream, SceneData& sceneData) {
        size_t material_count;
        stream.read(material_count);
        for (int i = 0; i < material_count; ++i) {
            std::string material_name;
            stream.read(material_name);

            // MaterialBuilder materialBuilder;
            // size_t          param_count;
            // stream.read(param_count);
            // for (int j = 0; j < param_count; ++j) {
            //     std::string param_name;
            //     stream.read(param_name);
            //     uint32_t param_type;
            //     stream.read(param_type);
            //     switch (param_type) {
            //         case 0: {
            //             ESamplerType sampler_type;
            //             stream.read(sampler_type);
            //             materialBuilder.SetParameter(param_name, sampler_type);
            //         } break;
            //         case 1: {
            //             ETextureDimension texture_type;
            //             stream.read(texture_type);
            //             materialBuilder.SetParameter(param_name, texture_type);
            //         } break;
            //         case 2: {
            //             UniformType type;
            //             stream.read(type);
            //             uint size;
            //             stream.read(size);
            //             materialBuilder.SetParameter(param_name, type, size);
            //         } break;
            //     }
            // }

            BufferInterfaceBlock block;
            {
                stream.read(block.m_name);
                stream.read(block.m_size);
                stream.read(block.m_alignment);
                stream.read(block.m_target);
                stream.read(block.m_qualifiers);

                size_t field_count;
                stream.read(field_count);
                block.m_field_info_list.resize(field_count);
                stream.read(block.m_field_info_list.data(), field_count * sizeof(BufferInterfaceBlock::FieldInfo));

                size_t info_count;
                stream.read(info_count);
                for (int j = 0; j < info_count; ++j) {
                    std::string name;
                    stream.read(name);
                    uint32_t offset;
                    stream.read(offset);
                    block.m_info_map[name] = offset;
                }
            }

            TextureInterfaceBlock sampler;
            {
                stream.read(sampler.m_name);
                size_t sampler_count;
                stream.read(sampler_count);
                sampler.m_sampler_info_list.resize(sampler_count);
                stream.read(sampler.m_sampler_info_list.data(), sampler_count * sizeof(TextureInterfaceBlock::TextureInfo));
                size_t info_count;
                stream.read(info_count);
                for (int j = 0; j < info_count; ++j) {
                    std::string name;
                    stream.read(name);
                    uint8_t offset;
                    stream.read(offset);
                    sampler.m_info_map[name] = offset;
                }
            }

            MaterialRef material = MoerNew(Material);
            material->SetBufferInterfaceBlock(block);
            material->SetSamplerInterfaceBlock(sampler);
            material->SetName(material_name);

            sceneData.m_materials[material_name] = material;
        }

        size_t material_instance_count;
        stream.read(material_instance_count);
        for (int i = 0; i < material_instance_count; ++i) {
            std::string material_instance_name;
            stream.read(material_instance_name);
            std::string mat_name;
            stream.read(mat_name);
            auto   material_instance = sceneData.m_materials[mat_name]->CreateInstance();
            size_t texture_param_count;
            stream.read(texture_param_count);
            for (int j = 0; j < texture_param_count; ++j) {
                std::string param_name;
                stream.read(param_name);
                std::string texture_name;
                stream.read(texture_name);
                sceneData.m_mat_instance_textures[material_instance_name].textures.emplace_back(param_name, texture_name);
            }
            //  sceneData.m_mat_instance_textures[material_instance_name].textures.resize(texture_param_count);
            //  stream.read(sceneData.m_mat_instance_textures[material_instance_name].textures);

            auto unfirom_buffer_size = material_instance->GetUniformBuffer().GetSize();
            auto buffer_data         = Moer::Array<uint8_t>(unfirom_buffer_size);
            stream.read(buffer_data.data(), unfirom_buffer_size);
            material_instance->SetUnifomBuffer(buffer_data.data(), unfirom_buffer_size);
            material_instance->SetName(material_instance_name);
            sceneData.m_material_instances[material_instance_name] = material_instance;
        }
    }

    void SceneCache::WriteSceneGeomInfo(OutputStream& stream, const SceneData& sceneData) {
        stream.write(sceneData.m_vertex_data.size());
        stream.write(sceneData.m_vertex_data.data(), sceneData.m_vertex_data.size() * sizeof(float));

        stream.write(sceneData.m_index_data.size());
        stream.write(sceneData.m_index_data.data(), sceneData.m_index_data.size() * sizeof(uint32_t));

        stream.write(sceneData.m_meshlet_descs.size());
        stream.write(sceneData.m_meshlet_descs.data(), sceneData.m_meshlet_descs.size() * sizeof(MeshletDesc));

        stream.write(sceneData.m_meshlet_bounds.size());
        stream.write(sceneData.m_meshlet_bounds.data(), sceneData.m_meshlet_bounds.size() * sizeof(MeshletBound));

        stream.write(sceneData.m_mesh_infos.size());
        stream.write(sceneData.m_mesh_infos.data(), sceneData.m_mesh_infos.size() * sizeof(MeshInfo));

        stream.write(sceneData.m_prim_infos.size());
        for (auto& prim_info : sceneData.m_prim_infos) {
            stream.write(prim_info.mesh_id);
            stream.write(prim_info.material_id);
            stream.write(prim_info.transform);
        }

        stream.write(sceneData.m_instance_data.size());
        stream.write(sceneData.m_instance_data.data(), sceneData.m_instance_data.size() * sizeof(InstanceData));

        stream.write(sceneData.m_instance_mesh_info.size());
        stream.write(sceneData.m_instance_mesh_info.data(), sceneData.m_instance_mesh_info.size() * sizeof(InstanceMeshInfo));

        stream.write(sceneData.m_instance_id.size());
        stream.write(sceneData.m_instance_id.data(), sceneData.m_instance_id.size() * sizeof(uint32_t));
    }
    void SceneCache::WriteSceneMaterial(OutputStream& stream, const SceneData& sceneData) {
        stream.write(sceneData.m_materials.size());
        for (auto& material : sceneData.m_materials) {
            stream.write(material.first);
            auto& sampler_info = material.second->GetSamplerInterfaceBlock();
            auto& buffer_info  = material.second->GetBufferInterfaceBlock();
            // auto& buffer_info_list  = buffer_info.GetFieldInfoList();
            // auto  sampler_info_list = sampler_info.GetSamplerInfoList();
            // stream.write(sampler_info_list.size() + buffer_info_list.size());
            // for (auto& sampler : sampler_info_list) {
            //     uint param_type = sampler.type == EParamaterType::SAMPLER ? 0 : 1;
            //     stream.write(sampler.name);
            //     stream.write(param_type);
            //     if (param_type == 0) {
            //         stream.write(sampler.samplerType);
            //     } else {
            //         stream.write(sampler.textureType);
            //     }
            // }
            // for (auto& buffer : buffer_info_list) {
            //     stream.write(buffer.name);
            //     stream.write(2);
            //     stream.write(buffer.type);
            //     stream.write(buffer.size);
            // }

            {
                stream.write(buffer_info.m_name);
                stream.write(buffer_info.m_size);
                stream.write(buffer_info.m_alignment);
                stream.write(buffer_info.m_target);
                stream.write(buffer_info.m_qualifiers);

                stream.write(buffer_info.m_field_info_list.size());
                stream.write(buffer_info.m_field_info_list.data(), buffer_info.m_field_info_list.size() * sizeof(BufferInterfaceBlock::FieldInfo));

                stream.write(buffer_info.m_info_map.size());
                for (auto& info : buffer_info.m_info_map) {
                    stream.write(std::string(info.first));
                    stream.write(info.second);
                }
            }

            {
                stream.write(sampler_info.m_name);
                stream.write(sampler_info.m_sampler_info_list.size());
                stream.write(sampler_info.m_sampler_info_list.data(), sampler_info.m_sampler_info_list.size() * sizeof(TextureInterfaceBlock::TextureInfo));
                stream.write(sampler_info.m_info_map.size());
                for (auto& info : sampler_info.m_info_map) {
                    stream.write(info.first);
                    stream.write(info.second);
                }
            }
        }

        stream.write(sceneData.m_material_instances.size());
        for (auto& material_instance : sceneData.m_material_instances) {
            stream.write(material_instance.first);
            stream.write(material_instance.second->GetMaterial()->GetName());

            stream.write(sceneData.m_mat_instance_textures.at(material_instance.first).textures.size());
            for (auto& texture : sceneData.m_mat_instance_textures.at(material_instance.first).textures) {
                stream.write(texture.first);
                stream.write(texture.second);
            }

            stream.write(material_instance.second->GetUniformBuffer().GetData(), material_instance.second->GetUniformBuffer().GetSize());
        }
    }
    void SceneCache::WriteSceneTextures(OutputStream& stream, const SceneData& sceneData) {
        stream.write(sceneData.m_textures.size());

        for (auto& texture : sceneData.m_textures) {
            stream.write(texture.first);
            stream.write(texture.second.width);
            stream.write(texture.second.height);
            stream.write(texture.second.layers);
            stream.write(texture.second.mips);
            stream.write(texture.second.channal);
            stream.write(texture.second.format);
            stream.write(texture.second.data_size);
            stream.write(texture.second.data.data(), texture.second.data_size);
            stream.write(texture.second.mip_offsets.data(), texture.second.mips * sizeof(uint32_t) * texture.second.layers);
            stream.write(texture.second.mip_extents.data(), texture.second.mips * sizeof(Extent3D) * texture.second.layers);
        }
    }
    void SceneCache::WriteSceneUtils(OutputStream& stream, const SceneData& sceneData) {
        stream.write(sceneData.m_cameras.size());
        for (auto& camera : sceneData.m_cameras) {
            stream.write(camera, sizeof(Camera));
        }
    }
    SceneData SceneCache::ConvertToSceneData(const Scene& scene) {
        //May be not necessary
        SceneData sceneData;
        return sceneData;
    }

    size_t HashSceneData(const SceneData& sceneData) {
        size_t hash = 0;
        return hash;
    }

    void SceneCache::Cache(const SceneData& sceneData, size_t key) {
        std::filesystem::path path = RemapScenePath(sceneData.m_path);
        std::ofstream         fs(path, std::ios::binary);
        OutputStream          stream(fs);
        WriteSceneGeomInfo(stream, sceneData);
        WriteSceneTextures(stream, sceneData);
        WriteSceneMaterial(stream, sceneData);
        WriteSceneUtils(stream, sceneData);
    }

    UniquePtr<Scene> SceneCache::ConvertToScene(SceneData& sceneData, bool need_cache) {
        size_t hash    = HashSceneData(sceneData);
        bool   updated = true;
        if (updated && need_cache) {
            Cache(sceneData, hash);
        }

        UniquePtr<Scene> scene = UniquePtr<Scene>(MoerNew(Scene));

        GpuSceneBufferBuilder buffer_builder;
        auto                  meshlet_bounds_buffer = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::UNORDERED_ACCESS, sceneData.m_meshlet_bounds.data(), sceneData.m_meshlet_bounds.size() * sizeof(MeshletBound));
        auto                  meshlet_descs_buffer  = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::UNORDERED_ACCESS, sceneData.m_meshlet_descs.data(), sceneData.m_meshlet_descs.size() * sizeof(MeshletDesc));
        //auto                  mesh_infos_buffer         = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::UNORDERED_ACCESS, sceneData.m_mesh_infos.data(), sceneData.m_mesh_infos.size() * sizeof(MeshInfo));
        auto instance_data_buffer      = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::UNORDERED_ACCESS, sceneData.m_instance_data.data(), sceneData.m_instance_data.size() * sizeof(InstanceData));
        auto vertex_buffer             = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::VERTEX_BUFFER, sceneData.m_vertex_data.data(), sceneData.m_vertex_data.size() * sizeof(float));
        auto index_buffer              = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::INDEX_BUFFER, sceneData.m_index_data.data(), sceneData.m_index_data.size() * sizeof(uint32_t));
        auto instance_id_buffer        = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::VERTEX_BUFFER, sceneData.m_instance_id.data(), sceneData.m_instance_id.size() * sizeof(uint32_t));
        auto instance_mesh_info_buffer = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::UNORDERED_ACCESS, sceneData.m_instance_mesh_info.data(), sceneData.m_instance_mesh_info.size() * sizeof(InstanceMeshInfo));

        scene->SetBuffer("meshlet_bounds", meshlet_bounds_buffer);
        scene->SetBuffer("meshlet_descs", meshlet_descs_buffer);
        // scene->SetBuffer("mesh_infos", mesh_infos_buffer);
        scene->SetBuffer("vertex_buffer", vertex_buffer);
        scene->SetBuffer("index_buffer", index_buffer);
        scene->SetBuffer("instance_data", instance_data_buffer);
        scene->SetBuffer("instance_id_buffer", instance_id_buffer);
        scene->SetBuffer("instance_meshlet_info_buffer", instance_mesh_info_buffer);

        for (auto& primitive : sceneData.m_prim_infos) {
            auto entity = EntityManager::Get().Create();
            scene->AddEntity(entity);
            RenderableManager::Get().Create(entity);
            RenderableManager::Get().SetMeshInfo(entity, sceneData.m_mesh_infos[primitive.mesh_id]);
            RenderableManager::Get().SetMaterialInstance(entity, sceneData.m_material_instances[primitive.material_id]);
        }

        for (auto& camera : sceneData.m_cameras) {
            auto entity = EntityManager::Get().Create();
            CameraManager::Get().Put(entity, camera);
            scene->AddCamera(entity);
        }

        Moer::UnorderedMap<std::string, RHITextureRef> textures;
        for (auto& texture : sceneData.m_textures) {
            TextureBuilder builder;
            builder.Data(texture.second.data.data(), texture.second.data.size());
            builder.Width(texture.second.width);
            builder.Height(texture.second.height);
            builder.Format(texture.second.format);
            builder.MipAndLayers(texture.second.mips, texture.second.layers, texture.second.mip_offsets.data(), texture.second.mip_extents.data());
            textures.emplace(texture.first, builder.Build());
        }

        for (auto material_instance : sceneData.m_material_instances) {
            for (auto& texture : sceneData.m_mat_instance_textures[material_instance.first].textures) {
                material_instance.second->SetParameter(texture.first, textures[texture.second]);
            }
        }

        return scene;
    }
    void SceneCache::LoadSceneFromCacheAsync(const std::filesystem::path& path) {
        LambdaTask::Dispatch([path]() {
            AsyncSceneLoadInfoRef load_info = MoerNew(AsyncSceneLoadInfo)();
            load_info->b_valid              = true;
            load_info->progress.store(0);
            Scene::RegisterAsyncLoadInfo(load_info);
            auto sceneData = FromFile(RemapScenePath(path)).release();
            EnqueueRenderTask([load_info = std::move(load_info), sceneData = std::move(sceneData)]() {
                load_info->scene = sceneData;
                Scene::SetCurrentScene(load_info->scene);
                load_info->progress.store(1);
            });
        });
    }

}