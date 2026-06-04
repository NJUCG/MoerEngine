// clang MSVC STL compat: MSVC STL headers (via pybind11) reference _invalid_parameter

#if defined(__clang__) && defined(_WIN32) && !defined(__clang_cl__)

#include <cstdint>

namespace { void _invalid_parameter(const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t){} }

#endif

#include "scripting/ScriptingModule.h"

// 修改这里的 pybind11 绑定时，也要同步维护同目录下的 stubs/moer/__init__.pyi。
// 这份 stub 主要给 VS Code / Pylance 做静态补全和签名提示，不参与运行时 import。

#if defined(_DEBUG)
#define MOER_SCRIPTING_MODULE_RESTORE_DEBUG 1
#undef _DEBUG
#endif
#include <Python.h>
#if defined(MOER_SCRIPTING_MODULE_RESTORE_DEBUG)
#define _DEBUG 1
#undef MOER_SCRIPTING_MODULE_RESTORE_DEBUG
#endif
#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include "math/Quaternion.h"
#include "math/Transform.h"
#include "misc/Traits.h"
#include "render/scene/Scene.h"
#include "scripting/SceneCall.h"

// 这个头文件只提供 entt::entity 的 pybind11 type_caster 特化
// => 如果别的 .cpp 里也直接进行了entt::entity 相关的 pybind11 绑定或 cast，
//    那个翻译单元也必须单独 include 这个头文件！
#include "scripting/PybindEnttEntityCaster.h" // IWYU pragma: keep

#include <atomic>
#include <format>
#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace Moer::scripting {

std::atomic<MainThreadCommandQueue*> g_active_scene_command_queue = nullptr;

MainThreadCommandQueue& RequireActiveSceneCommandQueue() {
    MainThreadCommandQueue* command_queue = g_active_scene_command_queue.load(std::memory_order_acquire);
    if (command_queue == nullptr) {
        throw std::runtime_error("Script scene API is not available.");
    }
    return *command_queue;
}

void BindFloat2(py::module_& module) {
    py::class_<float2> float2_class(module, "float2");
    float2_class.attr("__doc__") = "2D float vector.";
    float2_class.def(py::init<>())
        .def(py::init<float, float>(), py::arg("x"), py::arg("y"))
        .def_readwrite("x", &float2::x)
        .def_readwrite("y", &float2::y)
        .def("__repr__", [](const float2& value) {
            return std::string("float2") + value.ToString();
        });
}

void BindFloat3(py::module_& module) {
    py::class_<float3> float3_class(module, "float3");
    float3_class.attr("__doc__") = "3D float vector.";
    float3_class.def(py::init<>())
        .def(py::init<float, float, float>(), py::arg("x"), py::arg("y"), py::arg("z"))
        .def_readwrite("x", &float3::x)
        .def_readwrite("y", &float3::y)
        .def_readwrite("z", &float3::z)
        .def("__repr__", [](const float3& value) {
            return std::string("float3") + value.ToString();
        });
}

void BindFloat4(py::module_& module) {
    py::class_<float4> float4_class(module, "float4");
    float4_class.attr("__doc__") = "4D float vector.";
    float4_class.def(py::init<>())
        .def(py::init<float, float, float, float>(), py::arg("x"), py::arg("y"), py::arg("z"), py::arg("w"))
        .def_readwrite("x", &float4::x)
        .def_readwrite("y", &float4::y)
        .def_readwrite("z", &float4::z)
        .def_readwrite("w", &float4::w)
        .def("__repr__", [](const float4& value) {
            return std::string("float4") + value.ToString();
        });
}

void BindQuaternion(py::module_& module) {
    py::class_<Quaternion> quaternion_class(module, "Quaternion");
    quaternion_class.attr("__doc__") = "Quaternion value type.";
    quaternion_class.def(py::init<>())
        .def(py::init<float, float, float, float>(), py::arg("w"), py::arg("x"), py::arg("y"), py::arg("z"))
        .def_readwrite("vec", &Quaternion::vec)
        .def_property(
            "x",
            [](const Quaternion& value) {
                return value.vec.x;
            },
            [](Quaternion& value, float component) {
                value.vec.x = component;
            }
        )
        .def_property(
            "y",
            [](const Quaternion& value) {
                return value.vec.y;
            },
            [](Quaternion& value, float component) {
                value.vec.y = component;
            }
        )
        .def_property(
            "z",
            [](const Quaternion& value) {
                return value.vec.z;
            },
            [](Quaternion& value, float component) {
                value.vec.z = component;
            }
        )
        .def_property(
            "w",
            [](const Quaternion& value) {
                return value.vec.w;
            },
            [](Quaternion& value, float component) {
                value.vec.w = component;
            }
        )
        .def("__repr__", [](const Quaternion& value) {
            return std::format(
                "Quaternion(w={}, x={}, y={}, z={})", value.vec.w, value.vec.x, value.vec.y, value.vec.z
            );
        });
}

void BindAlphaMode(py::module_& module) {
    py::enum_<EAlphaMode>(module, "EAlphaMode")
        .value("Opaque", EAlphaMode::Opaque)
        .value("Mask", EAlphaMode::Mask)
        .value("Blend", EAlphaMode::Blend);
}

void BindProceduralPrimitiveShape(py::module_& module) {
    py::enum_<EProceduralPrimitiveShape>(module, "EProceduralPrimitiveShape")
        .value("Cube", EProceduralPrimitiveShape::Cube)
        .value("FacetedSphere", EProceduralPrimitiveShape::FacetedSphere);
}

void BindTransform(py::module_& module) {
    py::class_<Transform> transform_class(module, "Transform");
    transform_class.attr("__doc__") = "Affine transform value.";
    transform_class.def(py::init<>())
        .def(
            py::init<const float3&, const float3&, const Quaternion&>(),
            py::arg("translation"),
            py::arg("scale"),
            py::arg("rotation")
        )
        .def_property(
            "translation",
            [](const Transform& value) {
                return value.AffineDecomposition().translation;
            },
            [](Transform& value, const float3& translation) {
                const AffineTransformation affine = value.AffineDecomposition();
                value                             = Transform(translation, affine.scaling, affine.quaternion);
            }
        )
        .def_property(
            "scale",
            [](const Transform& value) {
                return value.AffineDecomposition().scaling;
            },
            [](Transform& value, const float3& scale) {
                const AffineTransformation affine = value.AffineDecomposition();
                value                             = Transform(affine.translation, scale, affine.quaternion);
            }
        )
        .def_property(
            "rotation",
            [](const Transform& value) {
                return value.AffineDecomposition().quaternion;
            },
            [](Transform& value, const Quaternion& rotation) {
                const AffineTransformation affine = value.AffineDecomposition();
                value                             = Transform(affine.translation, affine.scaling, rotation);
            }
        )
        .def("is_affine", &Transform::IsAffine)
        .def("__repr__", [](const Transform& value) {
            if (!value.IsAffine()) {
                return std::string("Transform(non_affine)");
            }

            const AffineTransformation affine = value.AffineDecomposition();
            return std::format(
                "Transform(translation={}, scale={}, rotation={})",
                affine.translation.ToString(),
                affine.scaling.ToString(),
                std::format(
                    "Quaternion(w={}, x={}, y={}, z={})",
                    affine.quaternion.vec.w,
                    affine.quaternion.vec.x,
                    affine.quaternion.vec.y,
                    affine.quaternion.vec.z
                )
            );
        });
}

void BindNodeLocalTransform(py::module_& module) {
    py::class_<Scene::NodeLocalTransform> transform_class(module, "NodeLocalTransform");
    transform_class.attr("__doc__") = "Node local transform value.";
    transform_class.def(py::init<>())
        .def_readwrite("translation", &Scene::NodeLocalTransform::translation)
        .def_readwrite("rotation", &Scene::NodeLocalTransform::rotation)
        .def_readwrite("scale", &Scene::NodeLocalTransform::scale);
}

void BindNodeSubtreeStats(py::module_& module) {
    py::class_<Scene::NodeSubtreeStats> subtree_stats_class(module, "NodeSubtreeStats");
    subtree_stats_class.attr("__doc__") = "Statistics for a node subtree.";
    subtree_stats_class.def(py::init<>())
        .def_readwrite("node_count", &Scene::NodeSubtreeStats::node_count)
        .def_readwrite("renderable_count", &Scene::NodeSubtreeStats::renderable_count)
        .def_readwrite("camera_count", &Scene::NodeSubtreeStats::camera_count)
        .def_readwrite("light_count", &Scene::NodeSubtreeStats::light_count)
        .def_readwrite("contains_main_camera", &Scene::NodeSubtreeStats::contains_main_camera)
        .def_readwrite("contains_main_light_tag", &Scene::NodeSubtreeStats::contains_main_light_tag);
}

void BindEntityComponentFlags(py::module_& module) {
    py::class_<Scene::EntityComponentFlags> component_flags_class(module, "EntityComponentFlags");
    component_flags_class.attr("__doc__") = "Lightweight component and tag flags for an entity handle.";
    component_flags_class.def(py::init<>())
        .def_readwrite("is_valid_entity", &Scene::EntityComponentFlags::is_valid_entity)
        .def_readwrite("is_node", &Scene::EntityComponentFlags::is_node)
        .def_readwrite("is_root_node", &Scene::EntityComponentFlags::is_root_node)
        .def_readwrite("is_renderable", &Scene::EntityComponentFlags::is_renderable)
        .def_readwrite("is_camera", &Scene::EntityComponentFlags::is_camera)
        .def_readwrite("is_light", &Scene::EntityComponentFlags::is_light)
        .def_readwrite("is_directional_light", &Scene::EntityComponentFlags::is_directional_light)
        .def_readwrite("is_point_light", &Scene::EntityComponentFlags::is_point_light)
        .def_readwrite("is_main_camera", &Scene::EntityComponentFlags::is_main_camera)
        .def_readwrite("is_main_light", &Scene::EntityComponentFlags::is_main_light);
}

void BindImportSceneFromFileResult(py::module_& module) {
    py::class_<Scene::ImportSceneFromFileResult> result_class(module, "SceneImportResult");
    result_class.attr("__doc__") = "Result of synchronously importing a scene file into the current scene.";
    result_class.def(py::init<>())
        .def_readwrite("success", &Scene::ImportSceneFromFileResult::success)
        .def_readwrite("error_message", &Scene::ImportSceneFromFileResult::error_message)
        .def_property_readonly(
            "import_root_entity",
            [](const Scene::ImportSceneFromFileResult& value) {
                return value.import_root_entt;
            }
        )
        .def_readwrite("imported_entity_count", &Scene::ImportSceneFromFileResult::imported_entity_count)
        .def("__bool__", [](const Scene::ImportSceneFromFileResult& value) {
            return static_cast<bool>(value);
        });
}

void BindPointLightCreateInfo(py::module_& module) {
    py::class_<PointLightCreateInfo> create_info_class(module, "PointLightCreateInfo");
    create_info_class.attr("__doc__") = "CreateInfo for a runtime point light.";
    create_info_class.def(py::init<>())
        .def_readwrite("position", &PointLightCreateInfo::position)
        .def_readwrite("color", &PointLightCreateInfo::color)
        .def_readwrite("intensity", &PointLightCreateInfo::intensity)
        .def_readwrite("name", &PointLightCreateInfo::name)
        .def_property(
            "parent_node_entity",
            [](const PointLightCreateInfo& value) {
                return value.parent_node_entt;
            },
            [](PointLightCreateInfo& value, entt::entity entity) {
                value.parent_node_entt = entity;
            }
        )
        .def_readwrite("should_set_main_light", &PointLightCreateInfo::should_set_main_light);
}

void BindEntityWithNodeCreateInfo(py::module_& module) {
    py::class_<EntityWithNodeCreateInfo> create_info_class(module, "EntityWithNodeCreateInfo");
    create_info_class.attr("__doc__") = "CreateInfo for an entity with a scene node.";
    create_info_class.def(py::init<>())
        .def_property(
            "parent_node_entity",
            [](const EntityWithNodeCreateInfo& value) {
                return value.parent_node_entt;
            },
            [](EntityWithNodeCreateInfo& value, entt::entity entity) {
                value.parent_node_entt = entity;
            }
        )
        .def_readwrite("name", &EntityWithNodeCreateInfo::name)
        .def_readwrite("translation", &EntityWithNodeCreateInfo::translation)
        .def_readwrite("rotation", &EntityWithNodeCreateInfo::rotation)
        .def_readwrite("scale", &EntityWithNodeCreateInfo::scale);
}

void BindMaterialCreateInfo(py::module_& module) {
    py::class_<MaterialCreateInfo> create_info_class(module, "MaterialCreateInfo");
    create_info_class.attr("__doc__") = "CreateInfo for a runtime material.";
    create_info_class.def(py::init<>())
        .def_readwrite("name", &MaterialCreateInfo::name)
        .def_readwrite("albedo_factor", &MaterialCreateInfo::albedo_factor)
        .def_readwrite("emissive_factor", &MaterialCreateInfo::emissive_factor)
        .def_readwrite("metallic_factor", &MaterialCreateInfo::metallic_factor)
        .def_readwrite("roughness_factor", &MaterialCreateInfo::roughness_factor)
        .def_readwrite("alpha_mode", &MaterialCreateInfo::alpha_mode)
        .def_readwrite("alpha_cutoff", &MaterialCreateInfo::alpha_cutoff);
}

void BindProceduralMeshCreateInfo(py::module_& module) {
    py::class_<ProceduralMeshCreateInfo> create_info_class(module, "ProceduralMeshCreateInfo");
    create_info_class.attr("__doc__") = "CreateInfo for a runtime procedural renderable.";
    create_info_class.def(py::init<>())
        .def_readwrite("shape", &ProceduralMeshCreateInfo::shape)
        .def_property(
            "parent_node_entity",
            [](const ProceduralMeshCreateInfo& value) {
                return value.parent_node_entt;
            },
            [](ProceduralMeshCreateInfo& value, entt::entity entity) {
                value.parent_node_entt = entity;
            }
        )
        .def_readwrite("name", &ProceduralMeshCreateInfo::name)
        .def_readwrite("translation", &ProceduralMeshCreateInfo::translation)
        .def_readwrite("rotation", &ProceduralMeshCreateInfo::rotation)
        .def_readwrite("scale", &ProceduralMeshCreateInfo::scale)
        .def_readwrite("material", &ProceduralMeshCreateInfo::material);
}

void BindCreateProceduralRenderableResult(py::module_& module) {
    py::class_<CreateProceduralRenderableResult> result_class(module, "CreateProceduralRenderableResult");
    result_class.attr("__doc__") = "Result of creating a runtime procedural renderable.";
    result_class.def(py::init<>())
        .def_property_readonly(
            "material_entity",
            [](const CreateProceduralRenderableResult& value) {
                return value.material_entt;
            }
        )
        .def_property_readonly(
            "primitive_entity",
            [](const CreateProceduralRenderableResult& value) {
                return value.primitive_entt;
            }
        )
        .def_property_readonly(
            "mesh_entity",
            [](const CreateProceduralRenderableResult& value) {
                return value.mesh_entt;
            }
        )
        .def_property_readonly(
            "renderable_entity",
            [](const CreateProceduralRenderableResult& value) {
                return value.renderable_entt;
            }
        )
        .def("__bool__", [](const CreateProceduralRenderableResult& value) {
            return static_cast<bool>(value);
        });
}

void BindSceneApi(py::module_& module) {
    py::class_<MainThreadCommandQueue> scene_api_class(module, "SceneApi");
    scene_api_class.attr("__doc__") = "Access the current runtime scene. Entity values are Python int "
                                      "handles and are not stable across scene reloads.";
    scene_api_class
        .def(
            "get_source_file_path",
            [](MainThreadCommandQueue& command_queue) {
                return CallScene(command_queue, [](Scene& scene) {
                    return scene.GetSourceFilePath().generic_string();
                });
            },
            "Return the current source scene path."
        )
        .def(
            "is_start_loading",
            [](MainThreadCommandQueue& command_queue) {
                return CallScene(command_queue, [](Scene& scene) {
                    return scene.IsStartLoading();
                });
            },
            "Return whether the scene has started loading."
        )
        .def(
            "is_ready",
            [](MainThreadCommandQueue& command_queue) {
                return CallScene(command_queue, [](Scene& scene) {
                    return scene.IsReady();
                });
            },
            "Return whether the scene is fully loaded and ready."
        )
        .def(
            "get_root_node_entity",
            [](MainThreadCommandQueue& command_queue) {
                return CallScene(command_queue, [](Scene& scene) {
                    return scene.GetRootNodeEntity();
                });
            },
            "Return the root node entity handle as a Python int."
        )
        .def(
            "is_valid_node_entity",
            [](MainThreadCommandQueue& command_queue, entt::entity entity) {
                return CallScene(command_queue, [entity](Scene& scene) {
                    return scene.IsValidNodeEntity(entity);
                });
            },
            py::arg("entity"),
            "Return whether the entity handle currently resolves to a valid scene node."
        )
        .def(
            "is_valid_entity",
            [](MainThreadCommandQueue& command_queue, entt::entity entity) {
                return CallScene(command_queue, [entity](Scene& scene) {
                    return scene.IsValidEntity(entity);
                });
            },
            py::arg("entity"),
            "Return whether the entity handle currently resolves to a live scene entity."
        )
        .def(
            "is_root_node",
            [](MainThreadCommandQueue& command_queue, entt::entity entity) {
                return CallScene(command_queue, [entity](Scene& scene) {
                    return scene.IsRootNode(entity);
                });
            },
            py::arg("entity"),
            "Return whether the entity is the scene root node."
        )
        .def(
            "get_node_child_count",
            [](MainThreadCommandQueue& command_queue, entt::entity entity) {
                return CallScene(command_queue, [entity](Scene& scene) {
                    return scene.GetNodeChildCount(entity);
                });
            },
            py::arg("entity"),
            "Return the direct child count for a node entity."
        )
        .def(
            "get_node_child_entity",
            [](MainThreadCommandQueue& command_queue, entt::entity entity, uint32 child_index) {
                return CallScene(command_queue, [entity, child_index](Scene& scene) {
                    return scene.GetNodeChildEntity(entity, child_index);
                });
            },
            py::arg("entity"),
            py::arg("child_index"),
            "Return the direct child node entity at child_index, or entt::null when out of range."
        )
        .def(
            "list_node_children",
            [](MainThreadCommandQueue& command_queue, entt::entity entity) {
                return CallScene(command_queue, [entity](Scene& scene) {
                    return scene.ListNodeChildren(entity);
                });
            },
            py::arg("entity"),
            "Return a Python list containing the direct child node entity handles."
        )
        .def(
            "get_entity_component_flags",
            [](MainThreadCommandQueue& command_queue, entt::entity entity) {
                return CallScene(command_queue, [entity](Scene& scene) {
                    return scene.GetEntityComponentFlags(entity);
                });
            },
            py::arg("entity"),
            "Return lightweight component and tag flags for an entity handle."
        )
        .def(
            "get_node_display_name",
            [](MainThreadCommandQueue& command_queue, entt::entity entity) {
                return CallScene(command_queue, [entity](Scene& scene) {
                    return scene.GetNodeDisplayName(entity);
                });
            },
            py::arg("entity"),
            "Return the display name for a node entity handle."
        )
        .def(
            "find_node_entity_by_name",
            [](MainThreadCommandQueue& command_queue, const std::string& name) {
                return CallScene(command_queue, [name](Scene& scene) {
                    return scene.FindNodeEntityByName(name);
                });
            },
            py::arg("name"),
            "Return the first node entity whose authored name or display name exactly matches name, or "
            "entt::null when not found."
        )
        .def(
            "get_node_subtree_stats",
            [](MainThreadCommandQueue& command_queue, entt::entity entity) {
                return CallScene(command_queue, [entity](Scene& scene) {
                    return scene.GetNodeSubtreeStats(entity);
                });
            },
            py::arg("entity"),
            "Return aggregate statistics for the node subtree."
        )
        .def(
            "try_get_node_name",
            [](MainThreadCommandQueue& command_queue, entt::entity entity) -> std::optional<std::string> {
                return CallScene(command_queue, [entity](Scene& scene) -> std::optional<std::string> {
                    std::string out_name;
                    if (!scene.TryGetNodeName(entity, out_name)) {
                        return std::nullopt;
                    }
                    return out_name;
                });
            },
            py::arg("entity"),
            "Return the authored node name for a valid node entity handle, or None if invalid."
        )
        .def(
            "try_get_node_local_transform",
            [](MainThreadCommandQueue& command_queue, entt::entity entity) {
                return CallScene(command_queue, [entity](Scene& scene) {
                    return scene.TryGetNodeLocalTransform(entity);
                });
            },
            py::arg("entity"),
            "Return the node local transform for a valid node entity handle, or None if invalid."
        )
        .def(
            "import_scene_from_file",
            [](MainThreadCommandQueue& command_queue, const std::string& file_path) {
                return CallScene(command_queue, [&file_path](Scene& scene) {
                    return scene.ImportSceneFromFileSync(file_path);
                });
            },
            py::arg("file_path"),
            "Synchronously import a scene file into the current scene and return the import result."
        )
        .def(
            "get_main_camera_entity",
            [](MainThreadCommandQueue& command_queue) {
                return CallScene(command_queue, [](Scene& scene) {
                    return scene.GetMainCameraEntity();
                });
            },
            "Return the main camera entity handle."
        )
        .def(
            "get_main_directional_light_entity",
            [](MainThreadCommandQueue& command_queue) {
                return CallScene(command_queue, [](Scene& scene) {
                    return scene.GetMainDirectionalLightEntity();
                });
            },
            "Return the main directional light entity handle."
        )
        .def(
            "get_main_point_light_entity",
            [](MainThreadCommandQueue& command_queue) {
                return CallScene(command_queue, [](Scene& scene) {
                    return scene.GetMainPointLightEntity();
                });
            },
            "Return the main point light entity handle."
        )
        .def(
            "create_entity",
            [](MainThreadCommandQueue& command_queue, const std::string& name) {
                return CallScene(command_queue, [name](Scene& scene) {
                    return scene.CreateEntity(name);
                });
            },
            py::arg("name") = std::string(),
            "Create a plain runtime entity without a scene node."
        )
        .def(
            "create_entity_with_node",
            [](MainThreadCommandQueue& command_queue, const EntityWithNodeCreateInfo& create_info) {
                return CallScene(command_queue, [create_info](Scene& scene) {
                    return scene.CreateEntityWithNode(create_info);
                });
            },
            py::arg("create_info"),
            "Create a runtime entity with a scene node."
        )
        .def(
            "create_point_light",
            [](MainThreadCommandQueue& command_queue, const PointLightCreateInfo& create_info) {
                return CallScene(command_queue, [create_info](Scene& scene) {
                    return scene.CreatePointLight(create_info);
                });
            },
            py::arg("create_info"),
            "Create a runtime point light."
        )
        .def(
            "create_procedural_renderable",
            [](MainThreadCommandQueue& command_queue, const ProceduralMeshCreateInfo& create_info) {
                return CallScene(command_queue, [create_info](Scene& scene) {
                    return scene.CreateProceduralRenderable(create_info);
                });
            },
            py::arg("create_info"),
            "Create a runtime procedural renderable and return all created entity handles."
        )
        .def(
            "set_node_name",
            [](MainThreadCommandQueue& command_queue, entt::entity entity, const std::string& name) {
                return CallScene(command_queue, [entity, name](Scene& scene) {
                    return scene.SetNodeName(entity, name);
                });
            },
            py::arg("entity"),
            py::arg("name"),
            "Set the node name for a valid node entity handle."
        )
        .def(
            "set_node_translation",
            [](MainThreadCommandQueue& command_queue, entt::entity entity, const float3& value) {
                return CallScene(command_queue, [entity, value](Scene& scene) {
                    return scene.SetNodeTranslation(entity, value);
                });
            },
            py::arg("entity"),
            py::arg("value"),
            "Set the local translation for a valid node entity handle."
        )
        .def(
            "set_node_rotation",
            [](MainThreadCommandQueue& command_queue, entt::entity entity, const Quaternion& value) {
                return CallScene(command_queue, [entity, value](Scene& scene) {
                    return scene.SetNodeRotation(entity, value);
                });
            },
            py::arg("entity"),
            py::arg("value"),
            "Set the local rotation for a valid node entity handle."
        )
        .def(
            "set_node_scale",
            [](MainThreadCommandQueue& command_queue, entt::entity entity, const float3& value) {
                return CallScene(command_queue, [entity, value](Scene& scene) {
                    return scene.SetNodeScale(entity, value);
                });
            },
            py::arg("entity"),
            py::arg("value"),
            "Set the local scale for a valid node entity handle."
        )
        .def(
            "set_local_transform",
            [](MainThreadCommandQueue& command_queue, entt::entity entity, const Transform& value) {
                return CallScene(command_queue, [entity, value](Scene& scene) {
                    return scene.SetLocalTransform(entity, value);
                });
            },
            py::arg("entity"),
            py::arg("value"),
            "Set the local transform for a valid node entity handle."
        )
        .def(
            "attach_to_parent",
            [](MainThreadCommandQueue& command_queue, entt::entity child_entity, entt::entity parent_entity) {
                return CallScene(command_queue, [child_entity, parent_entity](Scene& scene) {
                    return scene.AttachToParent(child_entity, parent_entity);
                });
            },
            py::arg("child_entity"),
            py::arg("parent_entity"),
            "Attach an existing node entity to a new parent node."
        )
        .def(
            "detach_from_parent",
            [](MainThreadCommandQueue& command_queue, entt::entity child_entity) {
                return CallScene(command_queue, [child_entity](Scene& scene) {
                    return scene.DetachFromParent(child_entity);
                });
            },
            py::arg("child_entity"),
            "Detach an existing node entity from its parent and reattach it to the root node."
        )
        .def(
            "destroy_entity",
            [](MainThreadCommandQueue& command_queue, entt::entity entity) {
                return CallScene(command_queue, [entity](Scene& scene) {
                    return scene.DestroyEntity(entity);
                });
            },
            py::arg("entity"),
            "Destroy a plain entity or a leaf node entity."
        )
        .def(
            "destroy_node_subtree",
            [](MainThreadCommandQueue& command_queue, entt::entity entity) {
                return CallScene(command_queue, [entity](Scene& scene) {
                    return scene.DestroyNodeSubtree(entity);
                });
            },
            py::arg("entity"),
            "Destroy a node and all of its descendants."
        )
        .def(
            "destroy_renderable",
            [](MainThreadCommandQueue& command_queue, entt::entity entity) {
                return CallScene(command_queue, [entity](Scene& scene) {
                    return scene.DestroyRenderable(entity);
                });
            },
            py::arg("entity"),
            "Destroy a renderable entity."
        )
        .def(
            "destroy_point_light",
            [](MainThreadCommandQueue& command_queue, entt::entity entity) {
                return CallScene(command_queue, [entity](Scene& scene) {
                    return scene.DestroyPointLight(entity);
                });
            },
            py::arg("entity"),
            "Destroy a runtime point light."
        );
}

void SetActiveSceneCommandQueue(MainThreadCommandQueue* command_queue) {
    g_active_scene_command_queue.store(command_queue, std::memory_order_release);
}

void ClearActiveSceneCommandQueue() {
    g_active_scene_command_queue.store(nullptr, std::memory_order_release);
}

} // namespace Moer::scripting

PYBIND11_EMBEDDED_MODULE(moer, module) {
    module.doc() = "Moer embedded scripting module. Entity values are exposed as Python int handles.";

    Moer::scripting::BindFloat2(module);
    Moer::scripting::BindFloat3(module);
    Moer::scripting::BindFloat4(module);
    Moer::scripting::BindAlphaMode(module);
    Moer::scripting::BindProceduralPrimitiveShape(module);
    Moer::scripting::BindQuaternion(module);
    Moer::scripting::BindTransform(module);
    Moer::scripting::BindNodeLocalTransform(module);
    Moer::scripting::BindNodeSubtreeStats(module);
    Moer::scripting::BindEntityComponentFlags(module);
    Moer::scripting::BindImportSceneFromFileResult(module);
    Moer::scripting::BindPointLightCreateInfo(module);
    Moer::scripting::BindEntityWithNodeCreateInfo(module);
    Moer::scripting::BindMaterialCreateInfo(module);
    Moer::scripting::BindProceduralMeshCreateInfo(module);
    Moer::scripting::BindCreateProceduralRenderableResult(module);
    Moer::scripting::BindSceneApi(module);

    module.def(
        "scene",
        []() -> Moer::scripting::MainThreadCommandQueue& {
            return Moer::scripting::RequireActiveSceneCommandQueue();
        },
        py::return_value_policy::reference,
        "Return the current SceneApi. Entity values are integer handles, not stable references."
    );
}