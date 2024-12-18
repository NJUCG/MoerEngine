#ifndef MOER_RT_RESOURCE_H
#define MOER_RT_RESOURCE_H

#include "misc/STL.h"
#include "rhi/RHIResource.h"
#include <filesystem>
#include <string_view>
namespace Moer::Render {
    class RTResource {
    public:
        RTResource(const std::filesystem::path& _resouce_path);
        ~RTResource();
        void LoadResources();
        void UnloadResources();

        TextureRef                                   GetTexture(std::string_view _name) const;
        BufferRef                                    GetBuffer(std::string_view _name) const;
        TextureRef                                   GetDefaultEnvMap();
        const UnorderedMap<std::string, TextureRef>& GetTextures() const { return textures; }

    private:
        bool                  b_loaded;
        std::filesystem::path resource_path;
        std::string_view      default_env_map_name;

        UnorderedMap<std::string, TextureRef> textures;
    };

    struct RTContext {
    public:
        RTContext(uint _num_emissive_meshes, uint _num_emissive_triangles, uint _num_prim_lights, uint _num_geom_instance, uint2 _env_map_extent);

        BufferRef geo_instance_to_light_buf;
        BufferRef light_mapping_buf;
        BufferRef prim_light_buf;
        BufferRef task_buf;
        BufferRef light_data_buf;

        TextureRef env_pdf_tex;
        TextureRef local_light_pdf_tex;

        uint max_emissive_meshes;
        uint max_emissive_triangles;
        uint max_geom_instance;
        uint max_prim_lights;
    };
}// namespace Moer::Render

#endif