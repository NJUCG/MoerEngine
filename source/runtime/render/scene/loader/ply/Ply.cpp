// #include "loader/ply/Ply.h"

// #include "misc/STL.h"
// #include "resources/GpuScene.h"
// #include "rhi/RHI.h"
// #include "scene/CameraManager.h"
// #include "scene/EntityManager.h"

// #include <fstream>
// #include <sstream>
// #include <stdexcept>
// #include <string>
// namespace Moer {
// struct PlyProperty {
//     std::string type;
//     std::string name;
// };

// struct PlyHeader {
//     std::string              format;
//     int                      numVertices;
//     int                      numFaces;
//     Moer::Array<PlyProperty> vertexProperties;
//     Moer::Array<PlyProperty> faceProperties;
// };

// class PlyLoader::Impl {
//     void loadPlyHeader(std::ifstream& plyFile);
//     void load();
//     void precomputeCov3D();

// public:
//     UniquePtr<Scene> loadSceneFromFile(const std::filesystem::path& file_path) {
//         filename = file_path.string();
//         m_scene  = std::move(UniquePtr<Scene>(MoerNew(Scene)()));
//         load();
//         return std::move(m_scene);
//     }

// protected:
//     PlyHeader        header{};
//     std::string      filename;
//     UniquePtr<Scene> m_scene{nullptr};
// };

// void PlyLoader::Impl::loadPlyHeader(std::ifstream& plyFile) {
//     if (!plyFile.is_open()) {
//         throw std::runtime_error("Could not open file: " + filename);
//     }

//     std::string line;
//     bool        headerEnd = false;

//     while (std::getline(plyFile, line)) {
//         std::istringstream iss(line);
//         std::string        token;

//         iss >> token;

//         if (token == "ply") {
//             // PLY format indicator
//         } else if (token == "format") {
//             iss >> header.format;
//         } else if (token == "element") {
//             iss >> token;

//             if (token == "vertex") {
//                 iss >> header.numVertices;
//             } else if (token == "face") {
//                 iss >> header.numFaces;
//             }
//         } else if (token == "property") {
//             PlyProperty property;
//             iss >> property.type >> property.name;

//             if (header.vertexProperties.size() < static_cast<size_t>(header.numVertices)) {
//                 header.vertexProperties.push_back(property);
//             } else {
//                 header.faceProperties.push_back(property);
//             }
//         } else if (token == "end_header") {
//             headerEnd = true;
//             break;
//         }
//     }

//     if (!headerEnd) {
//         throw std::runtime_error("Could not find end of header");
//     }
// }

// struct VertexStorage {
//     Vector3f position;
//     Vector3f normal;
//     float    shs[48];
//     float    opacity;
//     Vector3f scale;
//     Vector4f rotation;
// };

// struct Vertex {
//     Vector4f position;
//     Vector4f scale_opacity;
//     Vector4f rotation;
//     float    shs[48];
// };

// void PlyLoader::Impl::load() {
//     auto startTime = std::chrono::high_resolution_clock::now();

//     std::ifstream plyFile(filename, std::ios::binary);
//     loadPlyHeader(plyFile);

//     Moer::Array<Vertex> verteces(header.numVertices);
//     for (auto i = 0; i < header.numVertices; i++) {
//         static_assert(sizeof(VertexStorage) == 62 * sizeof(float));
//         assert(plyFile.is_open());
//         assert(!plyFile.eof());
//         VertexStorage vertexStorage;
//         plyFile.read(reinterpret_cast<char*>(&vertexStorage), sizeof(VertexStorage));

//         verteces[i].position = Vector4f(vertexStorage.position, 1.0f);
//         // verteces[i].normal = Vector4f(vertexStorage.normal, 0.0f);
//         verteces[i].scale_opacity =
//             Vector4f(Exp(vertexStorage.scale), 1.0f / (1.0f + std::exp(-vertexStorage.opacity)));
//         verteces[i].rotation = Normalizef(vertexStorage.rotation);
//         // memcpy(verteces[i].shs, vertexStorage.shs, 48 * sizeof(float));
//         verteces[i].shs[0] = vertexStorage.shs[0];
//         verteces[i].shs[1] = vertexStorage.shs[1];
//         verteces[i].shs[2] = vertexStorage.shs[2];
//         auto SH_N          = 16;
//         for (auto j = 1; j < SH_N; j++) {
//             verteces[i].shs[j * 3 + 0] = vertexStorage.shs[(j - 1) + 3];
//             verteces[i].shs[j * 3 + 1] = vertexStorage.shs[(j - 1) + SH_N + 2];
//             verteces[i].shs[j * 3 + 2] = vertexStorage.shs[(j - 1) + SH_N * 2 + 1];
//         }
//         assert(vertexStorage.normal.x == 0.0f);
//         assert(vertexStorage.normal.y == 0.0f);
//         assert(vertexStorage.normal.z == 0.0f);
//     }
//     Scene* scene = m_scene.get();
//     // EnqueueRenderTask([scene, verteces = std::move(verteces)]() {
//     //     auto gs_vertex_buffer = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::UNORDERED_ACCESS, verteces.data(), verteces.size() * sizeof(Vertex));
//     //     assert(false && "Not implemented");
//     //     // scene->SetBuffer("gs_scene_buffer", gs_vertex_buffer);
//     // });
//     auto      camera_entity = EntityManager::Get().Create();
//     auto      camera        = CameraManager::Get().Create(camera_entity);
//     Transform world_transform(Vector3f(0, 0, 5), Vector3f(1), Quaternion(1, 0, 0, 0));
//     float     tan_fovx = std::tan(Angle::DegreeToRadian(45.f) / 2.0);
//     float     tan_fovy = tan_fovx * 1080.f / 1920.f;
//     camera->Initialize(
//         world_transform,
//         Angle::RadianToDegree(std::atan(tan_fovy) * 2.0f),
//         1920.f / 1080.f,
//         0.1f,
//         1000.0f // default value, identical with gltf parser
//     );
//     m_scene->AddCamera(camera_entity);
//     precomputeCov3D();
// }

// void             PlyLoader::Impl::precomputeCov3D() {}
// UniquePtr<Scene> PlyLoader::LoadSceneFromFile(const std::filesystem::path& file_path) noexcept {
//     Impl impl;
//     return impl.loadSceneFromFile(file_path);
// }
// } // namespace Moer