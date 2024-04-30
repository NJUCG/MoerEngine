#ifndef MOERENGINE_SHADER_RESOURCE_MANAGER_H
#define MOERENGINE_SHADER_RESOURCE_MANAGER_H

#include "misc/CountableRef.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderMutation.h"
#include "shader/ShaderResource.h"
class GlobalShaderCache {
public:
    static GlobalShaderCache& GetInstance() {
        static GlobalShaderCache cache;
        return cache;
    }
    GlobalShaderCache();
    ~GlobalShaderCache();

    const ShaderCompilerOutput* FindShaderCache(EShaderPlatform platform, const ShaderResourceKey& key) const;

private:
    friend class ShaderResourceManager;
    void Load();
    void Dump();
    void UpdateOutput(Moer::Array<ShaderCompilerOutput*>& outputs);

private:
    struct Impl;
    Impl* impl;
};
class RENDER_API ShaderResourceManager {
public:
    static void                   Init(EShaderPlatform platform);
    static void                   ShutDown();
    static ShaderResourceManager& GetInstance();

    template<typename ShaderType>
        requires std::is_base_of_v<Shader, ShaderType>
    RHIShaderRef GetShader(const typename ShaderType::TMutationSet& _mutation_set) {
        const ShaderMetaType& meta_type = ShaderType::GetMetaType();

        if constexpr (ShaderType::TMutationSet::mutation_count == 0) {
            return GetShader(meta_type, 0);
        } else
            return GetShader(meta_type, _mutation_set.GetMutationID());
    }
    template<typename ShaderType>
        requires std::is_base_of_v<Shader, ShaderType>
    RHIShaderRef GetShader() {
        static_assert(ShaderType::TMutationSet::mutation_count == 1, "ShaderType should not have any mutation");
        const ShaderMetaType& meta_type = ShaderType::GetMetaType();

        return GetShader(meta_type, 0);
    }

    ShaderTypeResourceMap& GetShaderTypeMap() {
        return *type_resources;
    }

    void    PrepareGlobalShaderResources();
    Shader* GetShader(const ShaderMetaType& _meta_type);

private:
    friend class ShaderCompiler;
    ShaderResourceMap& GetShaderResourceMap() {
        return *shader_resources;
    }
    RHIShaderRef GetShader(const ShaderMetaType& _meta_type, uint32_t _mutation_id);
    friend Shader;
    ShaderResourceManager();
    ShaderTypeResourceMap* type_resources;
    ShaderResourceMap*     shader_resources;
};
#endif