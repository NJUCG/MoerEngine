#include "loader/jsonscene/JsonSceneParser.h"
#include "../io/ImageIO.h"
#include "../sceneCache/SceneCache.h"
#include "assimp/Importer.hpp"
#include "assimp/pbrmaterial.h"
#include "assimp/postprocess.h"
#include "assimp/scene.h"
#include "loader/jsonscene/CubeQuadPrimitive.h"
#include "math/Matrix.h"
#include "meshprocess/MeshProcessor.h"
#include "misc/STL.h"
#include "misc/Timer.h"
#include "misc/Traits.h"
#include "nlohmann/json.hpp"
#include "resources/GpuScene.h"
#include "rhi/RHI.h"
#include "scene/Material.h"
#include "scene/MaterialInstance.h"
#include "scene/Scene.h"
#include "taskgraph/GraphTask.h"
#include <RenderThread.h>
#include <fstream>
using Json = nlohmann::json;
using Path = std::filesystem::path;

namespace Moer {
template<NumericType T, size_t N>
    requires(N >= 2 && N <= 4)
void from_json(const nlohmann::json& j, Vector<T, N>& v) {
    if (j.is_number()) {
        for (size_t i = 0; i < N; ++i) {
            v[i] = j.get<T>();
        }
    } else {
        for (size_t i = 0; i < N; ++i) {
            v[i] = j.at(i).get<T>();
        }
    }
}

}; // namespace Moer

namespace Moer::Resource::JsonScene {
struct AttributeInfo {
    VertexAttributeFlags attribute;
    uint32_t             stride;
    uint32_t             attr_offset[4];
};

class EntityVertexData {
public:
    Array<float>    vertices;
    Array<uint32_t> indices;
    AttributeInfo   attribute_info;
    uint32_t        vertex_num;
};

class ExtendedSceneData {
public:
    UniquePtr<SceneData> scn_dat;
    uint32_t             vertex_cnt;
    uint32_t             index_cnt;
    uint32_t             meshlet_cnt;
    ExtendedSceneData() :
        scn_dat(UniquePtr<SceneData>(MoerNew(SceneData))),
        vertex_cnt(0),
        index_cnt(0),
        meshlet_cnt(0) {}
    ExtendedSceneData(const ExtendedSceneData&)            = delete;
    ExtendedSceneData& operator=(const ExtendedSceneData&) = delete;

    void PushbackMeshInfo(SharedPtr<MeshInfo> info, const MeshProcessOutput& output) {
        scn_dat->m_mesh_infos.push_back(info);
        // PushbackMeshlets(output);
        // scn_dat->m_vertex_data.insert(scn_dat->m_vertex_data.end(), output.meshlet_vertex_data.begin(), output.meshlet_vertex_data.end());
        // scn_dat->m_index_data.insert(scn_dat->m_index_data.end(), output.primitive_indices.begin(), output.primitive_indices.end());
        // vertex_cnt += info->vtx_count;
        // index_cnt += info->idx_count;
    }
    // void PushbackVertexData(const Array<float>& data) {
    //     scn_dat->m_vertex_data.insert(scn_dat->m_vertex_data.end(), data.begin(), data.end());
    // }
    // void PushbackIndexData(const Array<uint32_t>& data) {
    //     scn_dat->m_index_data.insert(scn_dat->m_index_data.end(), data.begin(), data.end());
    // }

    // void PushbackInstanceMeshInfo(const InstanceMeshInfo& info) {
    //     scn_dat->m_instance_mesh_info.push_back(info);
    // }

    void saveTextureData(
        const std::string&   rel_texture_path,
        const TextureData&   texture_data,
        MaterialInstanceRef& mat,
        const std::string&   param_name
    ) {
        scn_dat->m_textures[rel_texture_path] = texture_data;
        scn_dat->m_mat_instance_textures[mat->GetName()].textures.push_back({param_name, rel_texture_path});
    }
    void PushbackInstanceData(Matrix4x4f model2world, float max_scale, uint32_t padding) {
        uint32_t instance_id = static_cast<uint32_t>(scn_dat->m_instance_infos.size());

        //TODO: use new instance data
        float3x4             overload_m1 = Matrix3x4f{model2world.r0, model2world.r1, model2world.r2};
        Render::InstanceData data{.model2world = overload_m1, .prev_model2world = overload_m1};
        scn_dat->m_instance_infos.push_back(data);
    }
    // void PushbackPrimInfo(const uint32_t& mesh_id, const std::string& material_name, const Transform& transform) {
    //     scn_dat->m_prim_infos.emplace_back(mesh_id, material_name, transform);
    // }

private:
    // void PushbackMeshlets(const MeshProcessOutput& output) {
    //     scn_dat->m_meshlet_bounds.insert(scn_dat->m_meshlet_bounds.end(), output.meshlet_bounds.begin(), output.meshlet_bounds.end());
    //     scn_dat->m_meshlet_descs.insert(scn_dat->m_meshlet_descs.end(), output.meshlets.begin(), output.meshlets.end());
    //     meshlet_cnt += output.meshlets.size();
    // }
};

UniquePtr<EntityVertexData>    GetEntityVertexData(const aiMesh* mesh);
AttributeInfo                  GetAttribute(const aiMesh* mesh);
MeshProcessOutput              GenerateMeshlets(EntityVertexData& data);
TextureData                    CreateTextureData(const ImageReadDesc& image_desc);
Transform                      GetTransform(const Json& json);
Transform                      GetQuadTransform(const Json& quad_json);
std::tuple<Vector4f, Vector4f> GetTransformedAABB(MeshInfo& info, Matrix4x4f& model_2_world);

class JsonSceneParser::Impl {
public:
    UniquePtr<SceneData> LoadSceneFromFile(const Path& abs_scn_json_path, bool _delete_after_load = false);
    ~Impl() = default;

private:
    [[nodiscard("Handle Err")]] bool LoadEntities(ExtendedSceneData& dst, const Json& scene_json);
    [[nodiscard("Handle Err")]] bool LoadCamera(ExtendedSceneData& dst, const Json& camera_json);
    [[nodiscard("Handle Err")]] bool LoadMaterials(ExtendedSceneData& dst, const Json& scene_json);

    [[nodiscard("Handle Err")]] bool LoadEntityMeshData(ExtendedSceneData& dst, const Json& entity_json);
    void                             LoadObjFile(ExtendedSceneData& dst, const Path& abs_obj_path);
    void                             ProcessMesh(ExtendedSceneData& dst, const aiScene* mesh_scene);
    void                             LoadQuad(ExtendedSceneData& dst);
    void                             LoadCube(ExtendedSceneData& dst);
    void                             LoadLambertMaterial(ExtendedSceneData& dst, const Json& material_json);
    void                             LoadConductorMaterial(ExtendedSceneData& dst, const Json& material_json);
    void                             LoadNullMaterial(ExtendedSceneData& dst, const Json& material_json);
    void                             LoadTextureIntoMaterial(
                                    ExtendedSceneData&   dst,
                                    const Path&          abs_texture_path,
                                    MaterialInstanceRef& mat,
                                    const std::string&   param_name
                                );
    void InitDefaultMaterial(ExtendedSceneData& dst);

private:
    // This function will set both paths.
    void SetPaths(const Path& _abs_json_path) {
        this->abs_json_path  = _abs_json_path;
        abs_working_dir_path = _abs_json_path.parent_path();
    }
    Path abs_json_path, abs_working_dir_path;
};

JsonSceneParser::JsonSceneParser() noexcept {
    m_impl = MoerNew(JsonSceneParser::Impl);
}

JsonSceneParser::~JsonSceneParser() noexcept {
    if (m_impl) {
        MoerDelete(m_impl);
    }
}

RESOURCE_API UniquePtr<SceneData>
Moer::Resource::JsonScene::JsonSceneParser::LoadSceneFromFile(const Path& abs_scn_json_path) noexcept {
    Impl impl;
    return std::move(UniquePtr<SceneData>(impl.LoadSceneFromFile(abs_scn_json_path)));
}

UniquePtr<SceneData>
JsonSceneParser::Impl::LoadSceneFromFile(const Path& abs_scn_json_path, bool _delete_after_load) {

    LOG_ERROR("JSON Scene Loader needs a huge refactor. JSON scene is not supported yet.");
    assert(false);

    GpuPrimitiveBuilder::InitBuild();
    ExtendedSceneData ret_scene;
    auto              real_path = std::filesystem::weakly_canonical(abs_scn_json_path);
    if (!std::filesystem::exists(real_path)) {
        LOG_ERROR("Load Json Scene Failed: File not exist: {}", real_path.string());
        return nullptr;
    }
    std::ifstream ifs(real_path);
    Json          scene_json;
    try {
        scene_json = Json::parse(ifs);
        ifs.close();
    } catch (std::exception& e) {
        LOG_ERROR("Load Json Scene Failed (Ill-formed Json): {}", e.what());
        ifs.close();
        return nullptr;
    }
    SetPaths(real_path);
#ifdef _DEBUG
    ret_scene.scn_dat->m_path = real_path;
#endif
    if (!LoadMaterials(ret_scene, scene_json)) {
        LOG_ERROR("Load Json Scene Failed: Fatal Material Error.");
        return nullptr;
    }
    LOG_INFO("Json Scene Loader: Load Materials Successful.");
    if (!LoadCamera(ret_scene, scene_json.value("camera", Json::object()))) {
        LOG_ERROR("Load Json Scene Failed: Fatal Camera Error.");
        return nullptr;
    }
    LOG_INFO("Json Scene Loader: Load Camera Successful.");
    if (!LoadEntities(ret_scene, scene_json)) {
        LOG_ERROR("Load Json Scene Failed: Fatal Entities Error.");
        return nullptr;
    }
    LOG_INFO("Json Scene Loader: Load Entities Successful.");

    if (IsCurrentlyGameThread()) {
        RenderThreadFence fence;
        fence.Wait();
    }
    // FIXME: The following code will block the program. Maybe currently we have only one thread? Fix this in the future.
    // if (!IsCurrentlyRenderThread()) {
    //     ScopeEventRef event;
    //     EnqueueRenderTask([&event]() {
    //         event.Trigger();
    //     });
    // }
    return std::move(ret_scene.scn_dat);
}

[[nodiscard]] bool JsonSceneParser::Impl::LoadEntities(ExtendedSceneData& dst, const Json& scene_json) {
    if (!scene_json.contains("entities")) {
        LOG_WARNING("No entities found in {}.", abs_json_path.string());
        return true;
    }
    const auto& entities_json     = scene_json["entities"];
    Path        working_directory = abs_json_path.parent_path();
    for (auto& entity_json : entities_json) {
        if (!LoadEntityMeshData(dst, entity_json)) {
            continue;
        }
        uint32_t    mesh_id       = dst.scn_dat->m_mesh_infos.size() - 1;
        auto&       mesh_info     = dst.scn_dat->m_mesh_infos.back();
        std::string material_name = entity_json.value("material", "standard");
        Transform   transform     = GetTransform(entity_json.value("transform", Json::object()));
        if (entity_json["type"] == "quad") {
            transform = transform * GetQuadTransform(entity_json);
        }
        // dst.PushbackPrimInfo(mesh_id, material_name, transform);

        auto  model_2_world = transform.GetMatrix4x4();
        auto  scale         = transform.AffineDecomposition().scaling;
        float max_scale     = std::max(scale.x, std::max(scale.y, scale.z));
        dst.PushbackInstanceData(model_2_world, max_scale, 0);

        //TODO: new mesh info incomplete json loader
        // auto [new_aabb_min, new_aabb_max] = GetTransformedAABB(mesh_info, model_2_world);
        // InstanceMeshInfo instance_mesh_info{
        //     .center         = Vector3f(new_aabb_min + new_aabb_max) * 0.5f,
        //     .vertex_offset  = mesh_info.vertex_offset,
        //     .extent         = Vector3f(new_aabb_max - new_aabb_min) * 0.5f,
        //     .vertex_count   = mesh_info.vertex_count,
        //     .index_offset   = mesh_info.index_offset,
        //     .index_count    = mesh_info.index_count,
        //     .meshlet_offset = mesh_info.meshlet_offset,
        //     .meshlet_count  = mesh_info.meshlet_count};
        // dst.PushbackInstanceMeshInfo(instance_mesh_info);
    }

    return true;
}

bool Moer::Resource::JsonScene::JsonSceneParser::Impl::LoadEntityMeshData(
    ExtendedSceneData& dst,
    const Json&        entity_json
) {
    try {
        const auto& type = entity_json["type"];
        if (type == "mesh") {
            Path abs_mesh_path = abs_working_dir_path / entity_json["file"].get<std::string>();
            LoadObjFile(dst, abs_mesh_path);
        } else if (type == "quad") {
            LoadQuad(dst);
        } else if (type == "cube") {
            LoadCube(dst);
        } else {
            return false;
        }
    } catch (std::exception& e) {
        if (entity_json.contains("file")) {
            LOG_ERROR("Load entity({}) failed: {}", entity_json["file"].get<std::string>(), e.what());
        } else {
            LOG_ERROR("Load entity failed: {}. Possibly missing file path.", e.what());
        }
        return false;
    }
    return true;
}

void JsonSceneParser::Impl::LoadObjFile(ExtendedSceneData& dst, const Path& abs_obj_path) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_LINE | aiPrimitiveType_POINT);
    // clang-format off
        const auto* mesh_scene = importer.ReadFile(abs_obj_path.string(), 
            aiProcess_Triangulate
            | aiProcess_FlipUVs 
            | aiProcess_GenBoundingBoxes 
            | aiProcess_GenNormals 
            | aiProcess_CalcTangentSpace
            | aiProcess_SortByPType);
    // clang-format on
    if (!mesh_scene || mesh_scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !mesh_scene->mRootNode) {
        throw std::runtime_error(std::string("Error: ") + importer.GetErrorString());
    }
    ProcessMesh(dst, mesh_scene);
}

void Moer::Resource::JsonScene::JsonSceneParser::Impl::ProcessMesh(
    ExtendedSceneData& dst,
    const aiScene*     mesh_scene
) {
    for (uint32_t i = 0; i < mesh_scene->mNumMeshes; i++) {
        const auto* mesh        = mesh_scene->mMeshes[i];
        auto        data        = GetEntityVertexData(mesh);
        auto        output      = GenerateMeshlets(*data);
        auto        aabb_min    = mesh->mAABB.mMin;
        auto        aabb_max    = mesh->mAABB.mMax;
        auto        aabb_center = (mesh->mAABB.mMin + mesh->mAABB.mMax) * 0.5f;
        auto        aabb_extent = mesh->mAABB.mMax - aabb_center;
        //TODO: new mesh info incomplete json loader

        auto info = MakeShared<MeshInfo>();
        dst.PushbackMeshInfo(info, output);
    }
}

MeshProcessOutput GenerateMeshlets(EntityVertexData& data) {
    Moer::MeshProcessInput input{
        .vertex_data   = data.vertices.data(),
        .vertex_count  = data.vertex_num,
        .vertex_stride = data.attribute_info.stride * uint(sizeof(float)),
        .index_data    = data.indices.data(),
        .index_count   = static_cast<uint32_t>(data.indices.size())
    };
    return MeshProcessor::GenerateMeshlets(input);
}

UniquePtr<EntityVertexData> GetEntityVertexData(const aiMesh* mesh) {
    auto ret                = UniquePtr<EntityVertexData>(MoerNew(EntityVertexData));
    ret->attribute_info     = GetAttribute(mesh);
    const auto& stride      = ret->attribute_info.stride;
    const auto& attr_offset = ret->attribute_info.attr_offset;
    ret->vertex_num         = mesh->mNumVertices;
    ret->vertices.resize(ret->vertex_num * stride);
    for (uint32_t i = 0; i < ret->vertex_num; i++) {
        if (mesh->HasPositions()) {
            auto* const copy_src = reinterpret_cast<float*>(mesh->mVertices + i);
            std::copy(copy_src, copy_src + 3, ret->vertices.begin() + attr_offset[0] + i * stride);
        }
        if (mesh->HasNormals()) {
            auto* const copy_src = reinterpret_cast<float*>(mesh->mNormals + i);
            std::copy(copy_src, copy_src + 3, ret->vertices.begin() + attr_offset[1] + i * stride);
        }
        if (mesh->HasTangentsAndBitangents()) {
            auto* copy_src = reinterpret_cast<float*>(mesh->mTangents + i);
            std::copy(copy_src, copy_src + 3, ret->vertices.begin() + attr_offset[2] + i * stride);
        }
        if (mesh->HasTextureCoords(0)) {
            auto* const copy_src = reinterpret_cast<float*>(mesh->mTextureCoords[0] + i);
            std::copy(copy_src, copy_src + 2, ret->vertices.begin() + attr_offset[3] + i * stride);
        }
    }
    ret->indices.reserve(mesh->mNumFaces * 3);
    for (uint32_t i = 0; i < mesh->mNumFaces; i++) {
        const auto& face = mesh->mFaces[i];
        ret->indices.push_back(face.mIndices[0]);
        ret->indices.push_back(face.mIndices[1]);
        ret->indices.push_back(face.mIndices[2]);
    }
    return std::move(ret);
}

AttributeInfo GetAttribute(const aiMesh* mesh) {
    AttributeInfo      info          = {0, 0, {0, 0, 0, 0}};
    constexpr uint32_t POSITION_SIZE = 3;
    constexpr uint32_t NORMAL_SIZE   = 3;
    constexpr uint32_t TANGENT_SIZE  = 3;
    constexpr uint32_t UV_SIZE       = 2;
    if (mesh->HasPositions()) {
        info.attribute |= EVertexAttributeFlags::E_POSITION;
        info.attr_offset[0] = info.stride;
        info.stride += POSITION_SIZE;
    }
    if (mesh->HasNormals()) {
        info.attribute |= EVertexAttributeFlags::E_NORMAL;
        info.attr_offset[1] = info.stride;
        info.stride += NORMAL_SIZE;
    }
    if (mesh->HasTangentsAndBitangents()) {
        info.attribute |= EVertexAttributeFlags::E_TANGENT;
        info.attr_offset[2] = info.stride;
        info.stride += TANGENT_SIZE;
    }
    if (mesh->HasTextureCoords(0)) {
        info.attribute |= EVertexAttributeFlags::E_UV0;
        info.attr_offset[3] = info.stride;
        info.stride += UV_SIZE;
    }
    return info;
}

[[nodiscard]] bool JsonSceneParser::Impl::LoadCamera(ExtendedSceneData& dst, const Json& camera_json) {
    CameraRef camera = MoerNew(Camera)();
    // get fov
    float xfov = camera_json.value("fov", 36.f);
    // prepare position, look_at and up vector
    Vector3f position(0.f, 0.f, 0.f), look_at(0.0f, 0.0f, 1.0f), up(0.0f, 1.0f, 0.0f);
    if (camera_json.contains("transform")) {
        const Json& camera_trans_json = camera_json["transform"];
        position                      = camera_trans_json.value("position", position);
        // This look_at is a Point3f!
        look_at = camera_trans_json.value("look_at", look_at);
        up      = camera_trans_json.value("up", up);
    }
    // calculate transform
    Transform transform = Transform();
    // Transform accept look_at as a Point3f
    auto world_2_cam = Transform(position, look_at, up);
    transform.matrix = Inverse(world_2_cam.GetMatrix4x4());
    // get aspect ratio
    float aspect_ratio = 16.f / 9.f;
    if (camera_json.contains("resolution")) {
        Vector2i resolution;
        camera_json.at("resolution").get_to(resolution);
        aspect_ratio = static_cast<float>(resolution.x) / static_cast<float>(resolution.y);
    }
    // near clip is set to identical with Moer
    // load all the parameter into camera
    float distToFilm = 1.0f / tan(xfov * PI / 360);
    float yfov       = AI_RAD_TO_DEG(2 * atan(tan(AI_DEG_TO_RAD(xfov) / 2) / aspect_ratio));
    camera->Initialize(
        transform,
        yfov,
        aspect_ratio,
        distToFilm,
        1000.0f // default value, identical with gltf parser
    );
    dst.scn_dat->m_cameras.push_back(camera);
    return true;
}

bool JsonSceneParser::Impl::LoadMaterials(ExtendedSceneData& dst, const Json& scene_json) {
    if (!scene_json.contains("materials")) {
        LOG_WARNING("No materials found in {}.", abs_json_path.string());
        return true;
    }
    InitDefaultMaterial(dst);
    const auto& materials_json = scene_json["materials"];
    for (auto& material_json : materials_json) {
        try {
            const auto& type = material_json["type"].get<std::string>();
            if (type == "lambert") {
                LoadLambertMaterial(dst, material_json);
            } else if (type == "conductor") {
                LoadConductorMaterial(dst, material_json);
            } else if (type == "null") {
                LoadNullMaterial(dst, material_json);
            } else {
                LoadNullMaterial(dst, material_json);
            }
        } catch (std::exception& e) {
            if (material_json.contains("name")) {
                LOG_ERROR("Load material({}) failed: {}", material_json["name"].get<std::string>(), e.what());
            } else {
                LOG_ERROR("Load material failed: {}. Possibly missing 'name'.", e.what());
            }
            return false;
        }
    }

    return true;
}

void JsonSceneParser::Impl::InitDefaultMaterial(ExtendedSceneData& dst) {
    if (dst.scn_dat->m_materials.contains("standard")) {
        return;
    }
    MaterialBuilder material_builder;
    material_builder.SetParameter("base_color_factor", UniformType::FLOAT4);
    material_builder.SetParameter("emissive_factor", UniformType::FLOAT3);
    material_builder.SetParameter("metalic_factor", UniformType::FLOAT);
    material_builder.SetParameter("roughness_factor", UniformType::FLOAT);
    material_builder.SetParameter("ao", UniformType::FLOAT);

    material_builder.SetTexture("albedo_map", ETextureDimension::TEX_2D);
    material_builder.SetTexture("normal_map", ETextureDimension::TEX_2D);
    material_builder.SetTexture("metallic_roughness_map", ETextureDimension::TEX_2D);
    material_builder.SetTexture("ao_map", ETextureDimension::TEX_2D);
    material_builder.SetTexture("emissive_map", ETextureDimension::TEX_2D);
    material_builder.SetName("standard");

    dst.scn_dat->m_materials["standard"] = material_builder.Build();
}

static std::variant<std::string, Vector3f> GetAlbedoValue(const Json& lambert_material_json) {
    /* Four cases
        * 1. albedo texture path
        * 2. a single float value
        * 3. a three element array
        * 4. other
        */
    if (lambert_material_json.contains("albedo")) {
        const auto& albedo_json = lambert_material_json["albedo"];
        if (albedo_json.is_string()) {
            return albedo_json.get<std::string>();
        } else if (albedo_json.is_number()) {
            float v = albedo_json.get<float>();
            return Vector3f(v, v, v);
        } else if (albedo_json.is_array() && albedo_json.size() == 3) {
            Vector3f value(1.f, 1.f, 1.f);
            value = lambert_material_json.value("albedo", value);
            return value;
        }
    }
    return Vector3f(1.0f, 1.0f, 1.0f);
}

void JsonSceneParser::Impl::LoadLambertMaterial(ExtendedSceneData& dst, const Json& material_json) {
    if (!material_json.contains("name")) {
        LOG_ERROR("Invalid material format: Missing 'name' for a lambert material.");
        return;
    }
    std::string name     = material_json.at("name");
    const auto  material = dst.scn_dat->m_materials["standard"];
    if (dst.scn_dat->m_material_instance_indexes.contains(name)) {
        return;
    }
    MaterialInstanceRef mi = dst.scn_dat->m_material_instances.emplace_back(material->CreateInstance());
    dst.scn_dat->m_material_instance_indexes[name] = dst.scn_dat->m_material_instances.size() - 1;

    std::variant<std::string, Vector3f> albedo_value = GetAlbedoValue(material_json);
    if (std::holds_alternative<Vector3f>(albedo_value)) {
        mi->SetParameter("base_color_factor", Vector4f(std::get<Vector3f>(albedo_value), 1.f));
        mi->SetParameter("ao", 1.0f);
        return; // early return for code clarity
    }

    if (std::holds_alternative<std::string>(albedo_value)) {
        Path abs_texture_path = abs_working_dir_path / std::get<std::string>(albedo_value);
        LoadTextureIntoMaterial(dst, abs_texture_path, mi, "albedo_map");
        mi->SetParameter("ao", 1.0f);
        return;
    }
}

void JsonSceneParser::Impl::LoadConductorMaterial(ExtendedSceneData& dst, const Json& material_json) {
    if (!material_json.contains("name")) {
        LOG_ERROR("Invalid material format: Missing 'name' for a conductor material.");
        return;
    }
    std::string name     = material_json.at("name");
    const auto  material = dst.scn_dat->m_materials["standard"];
    if (dst.scn_dat->m_material_instance_indexes.contains(name)) {
        return;
    }
    MaterialInstanceRef mi = dst.scn_dat->m_material_instances.emplace_back(material->CreateInstance());
    dst.scn_dat->m_material_instance_indexes[name] = dst.scn_dat->m_material_instances.size() - 1;

    std::variant<std::string, Vector3f> albedo_value = GetAlbedoValue(material_json);
    if (std::holds_alternative<Vector3f>(albedo_value)) {
        mi->SetParameter("base_color_factor", Vector4f(std::get<Vector3f>(albedo_value), 1.f));
        float roughness = material_json.value("roughness", 0.5f);
        mi->SetParameter("roughness_factor", roughness);
        // Temperatory solution
        mi->SetParameter("metalic_factor", 1.0f);
        mi->SetParameter("ao", 1.0f);
        return;
    }
}

void JsonSceneParser::Impl::LoadNullMaterial(ExtendedSceneData& dst, const Json& material_json) {
    if (!material_json.contains("name")) {
        LOG_ERROR("Invalid material format: Missing 'name' for a conductor material.");
        return;
    }
    std::string name     = material_json.at("name");
    const auto  material = dst.scn_dat->m_materials["standard"];
    if (dst.scn_dat->m_material_instance_indexes.contains(name)) {
        return;
    }
    MaterialInstanceRef mi = dst.scn_dat->m_material_instances.emplace_back(material->CreateInstance());
    dst.scn_dat->m_material_instance_indexes[name] = dst.scn_dat->m_material_instances.size() - 1;

    Vector3f albedo_value(1.f, 1.f, 1.f);
    mi->SetParameter("base_color_factor", Vector4f(albedo_value, 1.f));
    float roughness = material_json.value("roughness", 1.f);
    mi->SetParameter("roughness_factor", roughness);
    // Temperatory solution
    mi->SetParameter("metalic_factor", 1.0f);
    mi->SetParameter("ao", 1.0f);
}

void JsonSceneParser::Impl::LoadTextureIntoMaterial(
    ExtendedSceneData&   dst,
    const Path&          abs_texture_path,
    MaterialInstanceRef& mat,
    const std::string&   param_name
) {
    // get a string used as the key in the texture map.
    // Using abs_texture_path as the key is also ok, but it is longer.
    std::string texture_key =
        (abs_texture_path.parent_path().filename() / abs_texture_path.filename()).string();
    if (dst.scn_dat->m_textures.contains(texture_key)) {
        auto& texture = dst.scn_dat->m_textures[texture_key];
        dst.scn_dat->m_mat_instance_textures[mat->GetName()].textures.push_back({param_name, texture_key});
        return;
    }
    ImageReadDesc image_desc = ImageIO::ReadFromFile(abs_texture_path);
    if (!image_desc.IsValid()) {
        LOG_ERROR("Load Texture Failed: {}", abs_texture_path.string());
        return;
    }
    TextureData texture_data = CreateTextureData(image_desc);
    dst.saveTextureData(texture_key, texture_data, mat, param_name);
    LOG_INFO("Load Texture Success: {}", abs_texture_path.string());
}

TextureData CreateTextureData(const ImageReadDesc& image_desc) {
    TextureData texture_data;
    texture_data.mips      = image_desc.mips;
    texture_data.layers    = image_desc.layers;
    texture_data.width     = image_desc.width;
    texture_data.height    = image_desc.height;
    texture_data.channal   = image_desc.channal;
    texture_data.format    = image_desc.format;
    texture_data.data_size = image_desc.data_size;
    texture_data.data.resize(image_desc.data_size);
    std::copy_n(reinterpret_cast<uint8_t*>(image_desc.data), image_desc.data_size, texture_data.data.data());
    texture_data.mip_offsets = image_desc.mip_offsets;
    texture_data.mip_extents = image_desc.mip_extents;

    if (image_desc.data_callback != nullptr) {
        image_desc.data_callback(image_desc.data);
    }
    return texture_data;
}

Quaternion EulerYZXToQuaternion(float pitch, float yaw, float roll) {
    float cy = cos(yaw * 0.5);
    float sy = sin(yaw * 0.5);
    float cp = cos(pitch * 0.5);
    float sp = sin(pitch * 0.5);
    float cr = cos(roll * 0.5);
    float sr = sin(roll * 0.5);

    float w = cr * cy * cp + sr * sy * sp;
    float x = cr * cy * sp - sr * sy * cp;
    float y = cr * sy * cp + sr * cy * sp;
    float z = sr * cy * cp - cr * sy * sp;
    return Quaternion(w, x, y, z);
}

Transform GetTransform(const Json& json) {
    Vector3f position(0.f, 0.f, 0.f), scale(1.f, 1.f, 1.f), rotation(0.f, 0.f, 0.f);
    position = json.value("position", position);
    scale    = json.value("scale", scale);
    rotation = json.value("rotation", rotation);

    Quaternion q =
        EulerYZXToQuaternion(AI_DEG_TO_RAD(rotation.x), AI_DEG_TO_RAD(rotation.y), AI_DEG_TO_RAD(rotation.z));
    return Transform(position, scale, q);
}

std::tuple<Vector4f, Vector4f> GetTransformedAABB(MeshInfo& info, Matrix4x4f& model_2_world) {
    Vector4f corner[8];
    // corner[0]    = model_2_world * Vector4f(info.center + info.extent, 1.0f);
    // corner[1]    = model_2_world * Vector4f(info.center - Vector3f(info.extent.x, info.extent.y, -info.extent.z), 1.0f);
    // corner[2]    = model_2_world * Vector4f(info.center - Vector3f(info.extent.x, -info.extent.y, info.extent.z), 1.0f);
    // corner[3]    = model_2_world * Vector4f(info.center - Vector3f(info.extent.x, -info.extent.y, -info.extent.z), 1.0f);
    // corner[4]    = model_2_world * Vector4f(info.center - Vector3f(-info.extent.x, info.extent.y, info.extent.z), 1.0f);
    // corner[5]    = model_2_world * Vector4f(info.center - Vector3f(-info.extent.x, info.extent.y, -info.extent.z), 1.0f);
    // corner[6]    = model_2_world * Vector4f(info.center - Vector3f(-info.extent.x, -info.extent.y, info.extent.z), 1.0f);
    // corner[7]    = model_2_world * Vector4f(info.center - info.extent, 1.0f);
    auto new_min = corner[0];
    auto new_max = corner[0];
    for (int i = 1; i < 8; i++) {
        new_min = Min(new_min, corner[i]);
        new_max = Max(new_max, corner[i]);
    }
    return std::tuple<Vector4f, Vector4f>(new_min, new_max);
}

void JsonSceneParser::Impl::LoadQuad(ExtendedSceneData& dst) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_LINE | aiPrimitiveType_POINT);
    // clang-format off
        const auto* mesh_scene = importer.ReadFileFromMemory(quad_mesh_obj.data(), 
            quad_mesh_obj.size(),
            aiProcess_Triangulate
            | aiProcess_FlipUVs 
            | aiProcess_GenBoundingBoxes 
            | aiProcess_GenNormals 
            | aiProcess_CalcTangentSpace
            | aiProcess_SortByPType);
    // clang-format on
    if (!mesh_scene || mesh_scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !mesh_scene->mRootNode) {
        throw std::runtime_error(std::string("Error: ") + importer.GetErrorString());
    }
    ProcessMesh(dst, mesh_scene);
}

void Moer::Resource::JsonScene::JsonSceneParser::Impl::LoadCube(ExtendedSceneData& dst) {
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_LINE | aiPrimitiveType_POINT);
    // clang-format off
        const auto* mesh_scene = importer.ReadFileFromMemory(cube_mesh_obj.data(), 
            cube_mesh_obj.size(),
            aiProcess_Triangulate
            | aiProcess_FlipUVs 
            | aiProcess_GenBoundingBoxes 
            | aiProcess_GenNormals 
            | aiProcess_CalcTangentSpace
            | aiProcess_SortByPType);
    // clang-format on
    if (!mesh_scene || mesh_scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !mesh_scene->mRootNode) {
        throw std::runtime_error(std::string("Error: ") + importer.GetErrorString());
    }
    ProcessMesh(dst, mesh_scene);
}

Transform GetQuadTransform(const Json& _quad_json) {
    // Because in loadQuad we actually only load a square plane
    // so we need the Transfrom that turn a square into the actual quad
    Vector3f base(0.f, 0.f, 0.f), e0(1.f, 0.f, 0.f), e1(0.f, 0.f, 1.f);
    base = _quad_json.value("base", base);
    e0   = _quad_json.value("edge0", e0);
    e1   = _quad_json.value("edge1", e1);
    Vector4f r0(e0.x, 0.f, e1.x, base.x - e0.x * 0.5 - e1.x * 0.5);
    Vector4f r1(e0.y, 1.f, e1.y, base.y - e0.y * 0.5 - e1.y * 0.5);
    Vector4f r2(e0.z, 0.f, e1.z, base.z - e0.z * 0.5 - e1.z * 0.5);
    Vector4f r3(0.f, 0.f, 0.f, 1.f);
    return Transform(Matrix4x4f(r0, r1, r2, r3));
}
} // namespace Moer::Resource::JsonScene