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

void BindNodeLocalTransform(py::module_& module) {
    py::class_<Scene::NodeLocalTransform> transform_class(module, "NodeLocalTransform");
    transform_class.attr("__doc__") = "Node local transform value.";
    transform_class.def(py::init<>())
        .def_readwrite("translation", &Scene::NodeLocalTransform::translation)
        .def_readwrite("rotation", &Scene::NodeLocalTransform::rotation)
        .def_readwrite("scale", &Scene::NodeLocalTransform::scale);
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

void BindSceneApi(py::module_& module) {
    py::class_<MainThreadCommandQueue> scene_api_class(module, "SceneApi");
    scene_api_class.attr("__doc__") = "Access the current runtime scene. Entity values are Python int "
                                      "handles and are not stable across scene reloads.";
    scene_api_class
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
                    return scene.IsValidNodeEntity(entity);
                });
            },
            py::arg("entity"),
            "Alias for is_valid_node_entity; use it before reusing a long-lived entity handle."
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
    Moer::scripting::BindQuaternion(module);
    Moer::scripting::BindNodeLocalTransform(module);
    Moer::scripting::BindImportSceneFromFileResult(module);
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