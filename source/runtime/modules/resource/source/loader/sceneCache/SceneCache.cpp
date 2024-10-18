#include "SceneCache.h"

#include "config/ConfigManager.h"
#include "misc/Timer.h"
#include "resources/GpuScene.h"
#include "rhi/RHI.h"
#include "scene/EntityManager.h"
#include "scene/SceneData.h"
#include "scene/light/LightComponentManager.h"
#include "scene/light/LightComponent.h"
#include "scene/light/DirectionalLightComponent.h"
#include "scene/light/PointLightComponent.h"
#include "scene/light/SpotLightComponent.h"
#include "taskgraph/GraphTask.h"

#include <fstream>
#include <filesystem>

#include <serialize/Serializer.h>
#include <span>
namespace Moer {

    class MaterialSystem {
    };

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

    static std::filesystem::path RemapScenePath(const std::filesystem::path& path) {
        return path.string() + ".MOERSCENE";
    }

    void SceneCache::FromFile(const std::filesystem::path& _path, Scene* _scene) {
        using namespace Moer;
        Timer timer;
        timer.Start();
        std::ifstream fs(_path, std::ios::binary);
        InputStream   stream(fs);

        SceneData scene_data;

        ReadSceneGeomInfo(stream, scene_data);
        ReadSceneTextures(stream, scene_data);
        ReadSceneMaterial(stream, scene_data);
        ReadSceneUtils(stream, scene_data);

        ConvertToScene(scene_data, _scene, false);
        timer.Stop();
        LOG_INFO("Load Scene Cache Time(ms): {}", timer.ElapsedMilliseconds());
    }
    bool SceneCache::HasValidCache(const std::filesystem::path& path) {
        auto cache_path = RemapScenePath(path);
        return std::filesystem::exists(cache_path);
    }
    void SceneCache::ToFile(const Scene& scene, const std::filesystem::path& path) {
    }
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
            // stream >> texture_data.channal;
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

#define DECLARE_AND_READ(type, name) \
    type name;                       \
    stream >> name

        DECLARE_AND_READ(size_t, light_count);
        _scene_data.m_lights.reserve(light_count);

        for (int i = 0; i < light_count; ++i) {
            DECLARE_AND_READ(ELightComponentType, type);
            DECLARE_AND_READ(Vector3f, color);
            DECLARE_AND_READ(float, intensity);

            if (type == ELightComponentType::DIRECTIONAL) {
                DECLARE_AND_READ(Vector3f, direction);

                auto light = MoerNew(DirectionalLightComponent)(
                    color,
                    intensity,
                    direction);
                _scene_data.m_lights.push_back(light);

            } else if (type == ELightComponentType::POINT) {
                DECLARE_AND_READ(Vector3f, position);

                auto light = MoerNew(PointLightComponent)(
                    color,
                    intensity,
                    position);
                _scene_data.m_lights.push_back(light);

            } else if (type == ELightComponentType::SPOT) {
                DECLARE_AND_READ(Vector3f, position);
                DECLARE_AND_READ(Vector3f, direction);
                DECLARE_AND_READ(float, inner_cone_angle);
                DECLARE_AND_READ(float, outer_cone_angle);

                auto light = MoerNew(SpotLightComponent)(
                    color,
                    intensity,
                    position,
                    direction,
                    inner_cone_angle,
                    outer_cone_angle);
                _scene_data.m_lights.push_back(light);

            } else {
                LOG_WARNING("Unknown light type: {}", static_cast<uint8_t>(type));
            }
        }
#undef DECLARE_AND_READ
    }

    void SceneCache::ReadSceneGeomInfo(FInputStream& stream, SceneData& sceneData) {
        size_t vertex_count;
        stream >> sceneData.m_vertex_data >> sceneData.m_index_data;
        // stream >> sceneData.m_index_data;
        // size_t meshlet_desc_count;
        // stream >> meshlet_desc_count;
        // sceneData.m_meshlet_descs.resize(meshlet_desc_count);
        // stream.Read(sceneData.m_meshlet_descs.data(), meshlet_desc_count * sizeof(MeshletDesc));
        stream >> sceneData.m_meshlet_descs;

        // size_t meshlet_bound_count;
        // stream >> meshlet_bound_count;
        // sceneData.m_meshlet_bounds.resize(meshlet_bound_count);
        // stream.Read(sceneData.m_meshlet_bounds.data(), meshlet_bound_count * sizeof(MeshletBound));

        // size_t mesh_info_count;
        // stream >> mesh_info_count;
        // sceneData.m_mesh_infos.resize(mesh_info_count);
        // stream.Read(sceneData.m_mesh_infos.data(), mesh_info_count * sizeof(MeshInfo));

        // size_t prim_info_count;
        // stream >> prim_info_count;
        // sceneData.m_prim_infos.resize(prim_info_count);
        // for (int i = 0; i < prim_info_count; ++i) {
        //     stream >> sceneData.m_prim_infos[i].mesh_id;
        //     stream >> sceneData.m_prim_infos[i].material_id;
        //     stream >> sceneData.m_prim_infos[i].transform;
        // }

        stream >> sceneData.m_meshlet_bounds >> sceneData.m_mesh_infos >> sceneData.m_prim_infos;

        // size_t instance_data_count;
        // stream >> instance_data_count;
        // sceneData.m_instance_data.resize(instance_data_count);
        // stream.Read(sceneData.m_instance_data.data(), instance_data_count * sizeof(InstanceData));

        // size_t instance_mesh_info_count;
        // stream >> instance_mesh_info_count;
        // sceneData.m_instance_mesh_info.resize(instance_mesh_info_count);
        // stream.Read(sceneData.m_instance_mesh_info.data(), instance_mesh_info_count * sizeof(InstanceMeshInfo));

        // size_t instance_id_count;
        // stream >> instance_id_count;
        // sceneData.m_instance_id.resize(instance_id_count);
        // stream.Read(sceneData.m_instance_id.data(), instance_id_count * sizeof(uint32_t));

        stream >> sceneData.m_instance_data >> sceneData.m_instance_mesh_info >> sceneData.m_instance_id;
    }

    void SceneCache::ReadSceneMaterial(InputStream& stream, SceneData& sceneData) {
        size_t material_count;
        stream >> material_count;
        for (int i = 0; i < material_count; ++i) {
            std::string material_name;
            // stream.Read(material_name);
            // stream >> sceneData.m_materials[material_name];
            stream >> material_name;
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
                // stream.Read(block.m_name);
                stream >> block.m_name;
                // stream.read(block.m_size);
                // stream.read(block.m_alignment);
                // stream.read(block.m_target);
                // stream.read(block.m_qualifiers);
                stream >> block.m_size;
                stream >> block.m_alignment;
                stream >> block.m_target;
                stream >> block.m_qualifiers;

                size_t field_count;
                // stream >> field_count;
                // block.m_field_info_list.resize(field_count);
                // stream.Read(block.m_field_info_list.data(), field_count * sizeof(BufferInterfaceBlock::FieldInfo));
                stream >> block.m_field_info_list;
                const auto& field_info_list = block.m_field_info_list;
                size_t      info_count;
                stream >> info_count;
                for (int j = 0; j < info_count; ++j) {
                    std::string name;
                    stream >> name;
                    uint32_t offset;
                    stream >> offset;
                    block.m_info_map[field_info_list[offset].name] = offset;
                }
            }

            TextureInterfaceBlock sampler;
            {
                stream >> sampler.m_name;
                // size_t sampler_count;
                // stream >> sampler_count;
                // sampler.m_sampler_info_list.resize(sampler_count);
                // stream.Read(sampler.m_sampler_info_list.data(), sampler_count * sizeof(TextureInterfaceBlock::TextureInfo));
                stream >> sampler.m_sampler_info_list;

                size_t info_count;
                stream >> info_count;
                for (int j = 0; j < info_count; ++j) {
                    std::string name;
                    stream >> name;
                    uint8_t offset;
                    stream >> offset;
                    sampler.m_info_map[sampler.m_sampler_info_list[offset].name] = offset;
                }
            }

            MaterialRef material = MoerNew(Material);
            material->SetBufferInterfaceBlock(block);
            material->SetSamplerInterfaceBlock(sampler);
            material->SetName(material_name);

            sceneData.m_materials[material_name] = material;
        }

        size_t material_instance_count;
        stream >> material_instance_count;
        for (int i = 0; i < material_instance_count; ++i) {
            std::string material_instance_name;
            stream >> material_instance_name;
            std::string mat_name;
            stream >> mat_name;
            auto   material_instance = sceneData.m_materials[mat_name]->CreateInstance();
            size_t texture_param_count;
            stream >> texture_param_count;
            for (int j = 0; j < texture_param_count; ++j) {
                std::string param_name;
                stream >> param_name;
                std::string texture_name;
                stream >> texture_name;
                sceneData.m_mat_instance_textures[material_instance_name].textures.emplace_back(param_name, texture_name);
            }
            //  sceneData.m_mat_instance_textures[material_instance_name].textures.resize(texture_param_count);
            //  stream.read(sceneData.m_mat_instance_textures[material_instance_name].textures);

            auto unfirom_buffer_size = material_instance->GetUniformBuffer().GetSize();
            auto buffer_data         = Moer::Array<uint8_t>(unfirom_buffer_size);
            stream >> buffer_data;
            material_instance->SetUnifomBuffer(buffer_data.data(), unfirom_buffer_size);
            material_instance->SetName(material_instance_name);
            sceneData.m_material_instances[material_instance_name]        = material_instance;
            sceneData.m_material_instance_indexes[material_instance_name] = i;
        }
    }

    void SceneCache::WriteSceneGeomInfo(FOutputStream& stream, const SceneData& sceneData) {
        stream << sceneData.m_vertex_data;

        // stream.write(sceneData.m_index_data.size());
        // stream.write(sceneData.m_index_data.data(), sceneData.m_index_data.size() * sizeof(uint32_t));

        // stream.write(sceneData.m_meshlet_descs.size());
        // stream.write(sceneData.m_meshlet_descs.data(), sceneData.m_meshlet_descs.size() * sizeof(MeshletDesc));

        // stream.write(sceneData.m_meshlet_bounds.size());
        // stream.write(sceneData.m_meshlet_bounds.data(), sceneData.m_meshlet_bounds.size() * sizeof(MeshletBound));

        // stream.write(sceneData.m_mesh_infos.size());
        // stream.write(sceneData.m_mesh_infos.data(), sceneData.m_mesh_infos.size() * sizeof(MeshInfo));

        stream << sceneData.m_index_data;
        stream << sceneData.m_meshlet_descs;
        stream << sceneData.m_meshlet_bounds;
        stream << sceneData.m_mesh_infos;

        stream << sceneData.m_prim_infos;
        // for (auto& prim_info : sceneData.m_prim_infos) {
        //     stream.write(prim_info.mesh_id);
        //     stream.write(prim_info.material_id);
        //     stream.write(prim_info.transform);
        // }

        // stream.write(sceneData.m_instance_data.size());
        // stream.write(sceneData.m_instance_data.data(), sceneData.m_instance_data.size() * sizeof(InstanceData));

        stream << sceneData.m_instance_data;

        // stream.write(sceneData.m_instance_mesh_info.size());
        // stream.write(sceneData.m_instance_mesh_info.data(), sceneData.m_instance_mesh_info.size() * sizeof(InstanceMeshInfo));

        // stream.write(sceneData.m_instance_id.size());
        // stream.write(sceneData.m_instance_id.data(), sceneData.m_instance_id.size() * sizeof(uint32_t));

        stream << sceneData.m_instance_mesh_info;
        stream << sceneData.m_instance_id;
    }
    void SceneCache::WriteSceneMaterial(FOutputStream& stream, const SceneData& sceneData) {
        // stream.write(sceneData.m_materials.size());
        stream << sceneData.m_materials.size();
        for (auto& material : sceneData.m_materials) {
            stream << material.first;
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
                // stream.write(buffer_info.m_name);
                // stream.write(buffer_info.m_size);
                // stream.write(buffer_info.m_alignment);
                // stream.write(buffer_info.m_target);
                // stream.write(buffer_info.m_qualifiers);

                // stream.write(buffer_info.m_field_info_list.size());
                // stream.write(buffer_info.m_field_info_list.data(), buffer_info.m_field_info_list.size() * sizeof(BufferInterfaceBlock::FieldInfo));
                stream << buffer_info.m_name;
                stream << buffer_info.m_size;
                stream << buffer_info.m_alignment;
                stream << buffer_info.m_target;
                stream << buffer_info.m_qualifiers;

                stream << buffer_info.m_field_info_list;

                // stream.write(buffer_info.m_info_map.size());
                for (auto& info : buffer_info.m_info_map) {
                    stream << info.first;
                    stream << info.second;
                    // stream.write(info.second);
                }
            }

            {
                // stream.write(sampler_info.m_name);
                // stream.write(sampler_info.m_sampler_info_list.size());
                // stream.write(sampler_info.m_sampler_info_list.data(), sampler_info.m_sampler_info_list.size() * sizeof(TextureInterfaceBlock::TextureInfo));
                // stream.write(sampler_info.m_info_map.size());
                // for (auto& info : sampler_info.m_info_map) {
                //     stream.write(info.first);
                //     stream.write(info.second);
                // }

                stream << sampler_info.m_name;
                stream << sampler_info.m_sampler_info_list;
                for (auto& info : sampler_info.m_info_map) {
                    stream << info.first;
                    stream << info.second;
                }
            }
        }

        // stream.write(sceneData.m_material_instances.size());
        stream << sceneData.m_material_instances.size();
        for (auto [name, index] : sceneData.m_material_instance_indexes) {
            auto& material_instance = sceneData.m_material_instances.at(name);
            // stream.write(name);
            // stream.write(material_instance->GetMaterial()->GetName());
            stream << name;
            stream << material_instance->GetMaterial()->GetName();

            if (sceneData.m_mat_instance_textures.contains(name)) {
                // stream.write(sceneData.m_mat_instance_textures.at(name).textures.size());
                // for (auto& texture : sceneData.m_mat_instance_textures.at(name).textures) {
                //     stream.write(texture.first);
                //     stream.write(texture.second);
                // }
                stream << sceneData.m_mat_instance_textures.at(name).textures;
                for (auto& texture : sceneData.m_mat_instance_textures.at(name).textures) {
                    stream << texture.first;
                    stream << texture.second;
                }
            } else {
                // Because in ReadSceneMaterial(), we expect the texture_param_count to be a size_t
                // Here we need to manual cast 0 to size_t to ensure it is written in 8 bytes instead of 4
                // stream.write(static_cast<size_t>(0));
                stream << 0ull;
            }

            stream << std::span<byte>((byte*)material_instance->GetUniformBuffer().GetData(), material_instance->GetUniformBuffer().GetSize());
            // stream << material_instance->GetUniformBuffer().GetData();
        }
    }
    void SceneCache::WriteSceneTextures(FOutputStream& stream, const SceneData& sceneData) {
        stream << sceneData.m_textures.size();
        // stream << sceneData.m_textures.size();

        for (auto& texture : sceneData.m_textures) {
            stream << texture.first;
            stream << texture.second;
        }

        // stream << sceneData.m_textures;
    }
    void SceneCache::WriteSceneUtils(OutputStream& stream, const SceneData& sceneData) {
        // write cameras
        // stream.write(sceneData.m_cameras.size());
        stream << sceneData.m_cameras.size();
        for (auto& camera : sceneData.m_cameras) {
            // stream.write(camera, sizeof(Camera));

            stream << camera;
        }

        // write lights
        // stream.write(sceneData.m_lights.size());

        stream << sceneData.m_lights.size();

        for (auto& light : sceneData.m_lights) {
            // stream.write(light->GetType());
            // stream.write(light->GetColor());
            // stream.write(light->GetIntensity());

            stream << light->GetType();
            stream << light->GetColor();
            stream << light->GetIntensity();

            if (light->GetType() == ELightComponentType::DIRECTIONAL) {
                auto* dir_light = dynamic_cast<DirectionalLightComponent*>(light.Get());
                // stream.write(dir_light->GetDirection());

                stream << dir_light->GetDirection();

            } else if (light->GetType() == ELightComponentType::POINT) {
                auto* point_light = dynamic_cast<PointLightComponent*>(light.Get());
                // stream.write(point_light->GetPosition());

                stream << point_light->GetPosition();

            } else if (light->GetType() == ELightComponentType::SPOT) {
                auto* spot_light = dynamic_cast<SpotLightComponent*>(light.Get());
                // stream.write(spot_light->GetPosition());
                // stream.write(spot_light->GetDirection());
                // stream.write(spot_light->GetInnerConeAngle());
                // stream.write(spot_light->GetOuterConeAngle());

                stream << spot_light->GetPosition();
                stream << spot_light->GetDirection();
                stream << spot_light->GetInnerConeAngle();
                stream << spot_light->GetOuterConeAngle();

            } else {
                LOG_WARNING("Unknown light type: {}", static_cast<uint8_t>(light->GetType()));
            }
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

    void BuildSceneRaytracing(SceneData& sceneData, Scene* scene) {

        Render::RaytracingSceneRef raytracing_scene = Render::RenderDevice::Get().CreateRaytracingScene();

        auto&               device = Render::RenderDevice::Get();
        Render::CommandList cmd_list;
        auto&               cmd_queue = device.GetCommandQueue(Render::EQueueType::Compute);

        Moer::Array<Render::RaytracingGeometryRef> blas_list;
        for (auto& primitive : sceneData.m_prim_infos) {
            Render::RaytracingGeometryInfo rt_geo_info{};
            const auto&                    mesh_info = sceneData.m_mesh_infos[primitive.mesh_id];
            rt_geo_info.build_flags                  = ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_TRACE;
            rt_geo_info.vertex_format                = PF_R32G32B32_SFLOAT;
            rt_geo_info.vertex_buffer                = scene->GetVertexBuffer();
            rt_geo_info.index_buffer                 = scene->GetIndexBuffer();
            rt_geo_info.index_type                   = IET_UINT32;
            rt_geo_info.max_vertex_count             = mesh_info.vertex_count;
            rt_geo_info.primitive_count              = mesh_info.index_count / 3;
            rt_geo_info.segments.emplace_back(mesh_info.vertex_offset, mesh_info.vertex_count, sceneData.m_vertex_stride, mesh_info.index_offset, mesh_info.index_count);

            Render::RaytracingGeometryRef blas = device.CreateRaytracingGeometry(rt_geo_info);
            cmd_list.BuildAccelerationStructures({{blas, ERaytracingBuildMode::BUILD}});

            Render::RaytracingMaterial  mat{};
            Render::RaytracingInstance& rt_instance = raytracing_scene->AddInstance();
            rt_instance.geom                        = blas;
            auto transform                          = primitive.transform.matrix;
            rt_instance.transform                   = Matrix3x4f(transform.r0, transform.r1, transform.r2);
            rt_instance.flag.need_create            = true;
            rt_instance.flag.need_update            = true;
            rt_instance.material_ref                = mat;

            rt_instance.visible_mask = Render::RTVM_ALL;
            blas_list.push_back(blas);
            raytracing_scene->RegisterGeometry(blas);

            // break;
        }
        // raytracing_scene->
        scene->SetRaytracingScene(raytracing_scene);
        cmd_list.UpdateRaytracingScene(raytracing_scene);
        cmd_queue.Execute(cmd_list.Submit());
        // cmd_queue.Sync();
        LOG_INFO("Build Scene Raytracing Completed");
        // Moer::Array<RHIRayTracingBLASRef> blas_list;
        // RHIRayTracingTLASInitializer tlas_initializer;
        //
        // for(auto& primitive : sceneData.m_prim_infos) {
        //     RHIRayTracingBLASInitializer blas_initializer;
        //     blas_initializer.build_flags = ERayTracingAccelerationStructureBuildFlags::ALLOW_UPDATE;
        //     auto & blas_geometryies = blas_initializer.geometries;
        //     auto & blas_range_infos = blas_initializer.range_infos;
        //
        //     auto & geometry = blas_geometryies.emplace_back();
        //     auto & triangles_geometry = geometry.geometry.triangles;
        //     triangles_geometry.vertex_buffer = scene->GetVertexBuffer();
        //     triangles_geometry.vertex_buffer_stride = sizeof(float) * (3+2+3);
        //     triangles_geometry.max_vertex_count = mesh_info.vertex_count;
        //     triangles_geometry.vertex_element_type = EPixelFormat::PF_R32G32B32_SFLOAT;
        //
        //     triangles_geometry.index_buffer = scene->GetIndexBuffer();
        //     triangles_geometry.index_element_type = EIndexElementType::IET_UINT32;
        //
        //     triangles_geometry.transform_buffer = GpuSceneBufferBuilder::CreateBufferWithData(EBufferUsageFlags::CONSTANT_BUFFER, &primitive.transform.matrix, sizeof(Moer::Matrix4x4f));
        //
        //     //todo Add aabb geometry
        //     geometry.geo_type = ERayTracingGeometryType::RTGT_TRIANGLES;
        //     geometry.flags = ERayTracingGeometryFlags::NONE;
        //
        //     auto & range_info = blas_range_infos.emplace_back();
        //     range_info.first_vertex = 0;
        //     range_info.primitive_count = mesh_info.index_count / 3;
        //     range_info.primtive_offset = 0;
        //     range_info.transform_offset = 0;
        //
        //     auto blas = g_rhi->RHIBuildRayTracingBLAS(blas_initializer);
        //     blas_list.push_back(blas);
        //
        //     auto & tlas_instance = tlas_initializer.instances.emplace_back();
        //     tlas_instance.blas = blas;
        //     tlas_instance.custom_index = primitive.mesh_id;
        //     tlas_instance.transform = primitive.transform.matrix;
        // }
        //
        // auto tlas = g_rhi->RHIBuildRayTracingTLAS(tlas_initializer);
        // scene->SetTlas(tlas);
        // scene->SetBlasList(blas_list);
    }

    void SceneCache::ConvertToScene(SceneData& scene_data, Scene* scene, bool need_cache) {
        size_t hash    = HashSceneData(scene_data);
        bool   updated = true;
        if (updated && need_cache) {
            Cache(scene_data, hash);
        }

        auto& device = Render::RenderDevice::Get();

        Moer::Array<byte> material_data(scene_data.m_material_instances.size() * Material::MaterialBytesNum);
        for (auto [name, index] : scene_data.m_material_instance_indexes) {
            auto& material = scene_data.m_material_instances[name];
            memcpy(material_data.data() + index * Material::MaterialBytesNum, material->GetUniformBuffer().GetData(), material->GetUniformBuffer().GetSize());
        }

        Render::CommandList cmd_list;
        auto&               cmd_queue      = device.GetCommandQueue(Render::EQueueType::Graphics);
        auto&               copy_queue     = device.GetCommandQueue(Render::EQueueType::Copy);
        auto                bindless_array = scene->GetBindlessArray();

        auto meshlet_bounds_buffer     = device.CreateBuffer<byte>(scene_data.m_meshlet_bounds.size() * sizeof(MeshletBound), EBufferUsageFlags::UNORDERED_ACCESS);
        auto meshlet_descs_buffer      = device.CreateBuffer<byte>(scene_data.m_meshlet_descs.size() * sizeof(MeshletDesc), EBufferUsageFlags::UNORDERED_ACCESS);
        auto mesh_infos_buffer         = device.CreateBuffer<byte>(scene_data.m_mesh_infos.size() * sizeof(MeshInfo), EBufferUsageFlags::UNORDERED_ACCESS);
        auto instance_data_buffer      = device.CreateBuffer<byte>(scene_data.m_instance_data.size() * sizeof(InstanceData), EBufferUsageFlags::UNORDERED_ACCESS);
        auto vertex_buffer             = device.CreateBuffer<float>(scene_data.m_vertex_data.size(), EBufferUsageFlags::VERTEX_BUFFER | EBufferUsageFlags::ACCELERATION_STRUCTURE);
        auto index_buffer              = device.CreateBuffer<uint32_t>(scene_data.m_index_data.size(), EBufferUsageFlags::INDEX_BUFFER | EBufferUsageFlags::ACCELERATION_STRUCTURE);
        auto instance_id_buffer        = device.CreateBuffer<uint32_t>(scene_data.m_instance_id.size(), EBufferUsageFlags::VERTEX_BUFFER);
        auto instance_mesh_info_buffer = device.CreateBuffer<byte>(scene_data.m_instance_mesh_info.size() * sizeof(InstanceMeshInfo), EBufferUsageFlags::UNORDERED_ACCESS);
        auto material_buffer           = device.CreateBuffer<byte>(scene_data.m_material_instances.size() * Material::MaterialBytesNum, EBufferUsageFlags::UNORDERED_ACCESS);

        cmd_list.CopyFrom(std::span<byte>((byte*)scene_data.m_meshlet_bounds.data(), scene_data.m_meshlet_bounds.size() * sizeof(MeshletBound)), meshlet_bounds_buffer->GetView());
        cmd_list.CopyFrom(std::span<byte>((byte*)scene_data.m_meshlet_descs.data(), scene_data.m_meshlet_descs.size() * sizeof(MeshletDesc)), meshlet_descs_buffer->GetView());
        cmd_list.CopyFrom(std::span<byte>((byte*)scene_data.m_mesh_infos.data(), scene_data.m_mesh_infos.size() * sizeof(MeshInfo)), mesh_infos_buffer->GetView());
        cmd_list.CopyFrom(std::span<byte>((byte*)scene_data.m_instance_data.data(), scene_data.m_instance_data.size() * sizeof(InstanceData)), instance_data_buffer->GetView());
        cmd_list.CopyFrom(std::span<byte>((byte*)scene_data.m_vertex_data.data(), scene_data.m_vertex_data.size() * sizeof(float)), vertex_buffer->GetView());
        cmd_list.CopyFrom(std::span<byte>((byte*)scene_data.m_index_data.data(), scene_data.m_index_data.size() * sizeof(uint32_t)), index_buffer->GetView());
        cmd_list.CopyFrom(std::span<byte>((byte*)scene_data.m_instance_id.data(), scene_data.m_instance_id.size() * sizeof(uint32_t)), instance_id_buffer->GetView());
        cmd_list.CopyFrom(std::span<byte>((byte*)scene_data.m_instance_mesh_info.data(), scene_data.m_instance_mesh_info.size() * sizeof(InstanceMeshInfo)), instance_mesh_info_buffer->GetView());
        cmd_list.CopyFrom(material_data, material_buffer->GetView());

        copy_queue.Execute(cmd_list.Submit());
        copy_queue.Sync();

        // GpuSceneBufferBuilder buffer_builder;
        // auto                  meshlet_bounds_buffer = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::UNORDERED_ACCESS, sceneData.m_meshlet_bounds.data(), sceneData.m_meshlet_bounds.size() * sizeof(MeshletBound));
        // auto                  meshlet_descs_buffer  = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::UNORDERED_ACCESS, sceneData.m_meshlet_descs.data(), sceneData.m_meshlet_descs.size() * sizeof(MeshletDesc));
        // //auto                  mesh_infos_buffer         = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::UNORDERED_ACCESS, sceneData.m_mesh_infos.data(), sceneData.m_mesh_infos.size() * sizeof(MeshInfo));
        // auto instance_data_buffer      = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::UNORDERED_ACCESS, sceneData.m_instance_data.data(), sceneData.m_instance_data.size() * sizeof(InstanceData));
        // auto vertex_buffer             = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::VERTEX_BUFFER, sceneData.m_vertex_data.data(), sceneData.m_vertex_data.size() * sizeof(float));
        // auto index_buffer              = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::INDEX_BUFFER, sceneData.m_index_data.data(), sceneData.m_index_data.size() * sizeof(uint32_t));
        // auto instance_id_buffer        = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::VERTEX_BUFFER, sceneData.m_instance_id.data(), sceneData.m_instance_id.size() * sizeof(uint32_t));
        // auto instance_mesh_info_buffer = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::UNORDERED_ACCESS, sceneData.m_instance_mesh_info.data(), sceneData.m_instance_mesh_info.size() * sizeof(InstanceMeshInfo));

        // scene->SetBuffer("meshlet_bounds", meshlet_bounds_buffer);
        // scene->SetBuffer("meshlet_descs", meshlet_descs_buffer);
        // // scene->SetBuffer("mesh_infos", mesh_infos_buffer);
        // scene->SetBuffer("vertex_buffer", vertex_buffer);
        // scene->SetBuffer("index_buffer", index_buffer);
        // scene->SetBuffer("instance_data", instance_data_buffer);
        // scene->SetBuffer("instance_id_buffer", instance_id_buffer);
        // scene->SetBuffer("instance_meshlet_info_buffer", instance_mesh_info_buffer);
        scene->SetVertexBuffer(vertex_buffer);
        scene->SetIndexBuffer(index_buffer);
        scene->SetBuffer(EGpuSceneResource::InstanceInfo, instance_data_buffer);
        scene->SetBuffer(EGpuSceneResource::MaterialInfo, material_buffer);

        for (auto& primitive : scene_data.m_prim_infos) {
            auto entity = EntityManager::Get().Create();
            scene->AddEntity(entity);
            RenderableManager::Get().Create(entity);
            RenderableManager::Get().SetMeshInfo(entity, scene_data.m_mesh_infos[primitive.mesh_id]);
            RenderableManager::Get().SetMaterialInstance(entity, scene_data.m_material_instances[primitive.material_id]);
        }

        for (auto& camera : scene_data.m_cameras) {
            auto entity = EntityManager::Get().Create();
            CameraManager::Get().Put(entity, camera);
            scene->AddCamera(entity);
        }

        for (auto& light : scene_data.m_lights) {
            auto entity = EntityManager::Get().Create();
            LightComponentManager::Get().Put(entity, light);
            scene->AddLight(entity);
        }

        Moer::UnorderedMap<std::string, Render::TextureRef> textures;
        Moer::Array<TextureBuilder>                         texture_builders;
        texture_builders.reserve(scene_data.m_textures.size());
        for (auto& texture : scene_data.m_textures) {
            auto& builder = texture_builders.emplace_back();
            builder.Data(texture.second.data.data(), texture.second.data.size());
            builder.Width(texture.second.width);
            builder.Height(texture.second.height);
            builder.Format(texture.second.format);
            builder.MipAndLayers(texture.second.mips, texture.second.layers, texture.second.mip_offsets.data(), texture.second.mip_extents.data());
            builder.Name(texture.first);
        }
        textures = TextureBuilder::BuildTexturesInBatch(texture_builders);
        Render::Sampler sampler(SF_LINEAR, SAM_REPEAT);

        //  scene_data.m_textures = GpuSceneBufferBuilder::
        for (auto material_instance : scene_data.m_material_instances) {
            if (!scene_data.m_mat_instance_textures.contains(material_instance.first)) {
                continue;
            }
            for (auto& texture : scene_data.m_mat_instance_textures[material_instance.first].textures) {
                uint32_t handle = scene->GetBindlessArray()->AllocateTexture(textures[texture.second], sampler);
                material_instance.second->SetParameter(texture.first, handle);
            }
        }
        // BuildSceneRaytracing(scene_data,scene.get());
    }
    void SceneCache::LoadSceneFromCacheAsync(const std::filesystem::path& path, Scene* scene) {
        LambdaTask::Dispatch([path, scene]() {
            AsyncSceneLoadInfoRef load_info = MoerNew(AsyncSceneLoadInfo)();
            load_info->b_valid              = true;
            load_info->progress.store(0);
            Scene::RegisterAsyncLoadInfo(load_info);
            FromFile(RemapScenePath(path), scene);
            // load_info->scene = scene_data;
            // Scene::SetCurrentScene(load_info->scene);
            load_info->progress.store(1);
        });
    }
    void SceneCache::LoadSceneFromCache(const std::filesystem::path& path, Scene* scene) {
        AsyncSceneLoadInfoRef load_info = MoerNew(AsyncSceneLoadInfo)();
        load_info->b_valid              = true;
        load_info->progress.store(0);
        Scene::RegisterAsyncLoadInfo(load_info);
        FromFile(RemapScenePath(path), scene);
        // load_info->scene = scene_data;
        // Scene::SetCurrentScene(load_info->scene);
        load_info->progress.store(1);
    }

}// namespace Moer