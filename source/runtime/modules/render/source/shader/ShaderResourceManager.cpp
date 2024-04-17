#include "shader/ShaderResourceManager.h"
#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "misc/Hash.h"
#include "misc/STL.h"
#include "misc/Timer.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderResource.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"

#include <sstream>
#include <zpp_bits.h>
#include <filesystem>
#include <fstream>
#include <ranges>
struct GlobalShaderCacheBundle {
    // Moer::UnorderedMap<ShaderResourceKey, ShaderCompilerOutput> shader_cache;
    Moer::UnorderedMap<ShaderResourceKey, uint32_t> shader_cache_index;
    Moer::Array<ShaderCompilerOutput>               shader_cache_array;

    bool IsEmpty() const {
        return shader_cache_index.empty();
    }
    // auto serialize(const GlobalShaderCacheBundle& bundle) -> zpp::bits::members<2>;
};

struct GlobalShaderCache::Impl {
public:
    void Load();
    //read only output(during compilation, after that returned value may be invalidated)
    const ShaderCompilerOutput* FindShaderCache(EShaderPlatform platform, const ShaderResourceKey& key) const {
        auto& cache = shader_cache[platform];
        auto  it    = cache.shader_cache_index.find(key);
        if (it != cache.shader_cache_index.end()) {
            return &cache.shader_cache_array[it->second];
        }
        return nullptr;
    }

    //must call after compile
    void UpdateOutput(Moer::Array<ShaderCompilerOutput*>& outputs) {
        for (auto& output : outputs) {
            auto& cache    = shader_cache[output->target_info.shader_platform];
            output->cached = true;

            ShaderResourceKey key{output->shader_name_hash, output->mutation_id};

            auto it = cache.shader_cache_index.find(key);
            if (it != cache.shader_cache_index.end()) {
                cache.shader_cache_array[it->second] = *output;
            } else {
                cache.shader_cache_index[key] = cache.shader_cache_array.size();
                cache.shader_cache_array.push_back(*output);
            }
        }
    }
    void Dump();
    friend zpp::bits::access;

private:
    GlobalShaderCacheBundle shader_cache[EShaderPlatform::SP_Num];
};

constexpr static auto serialize(auto& archive, ShaderCompilerOutput& self) {
    return archive(self.parameter_map,
                   self.errors,
                   self.pragma,
                   self.target_info,
                   self.shader_code,
                   self.compiled_hash,
                   self.num_instructions,
                   self.num_samplers,
                   self.mutation_id,
                   self.compiled_time,
                   self.preprocessing_time,
                   self.b_succeeded,
                   self.shader_name_hash,
                   self.cached,
                   self.source_file_last_write_time);
}

constexpr static auto serialize(auto& archive, ShaderTargetInfo& self) {
    return archive(self.shader_type, self.shader_platform);
}

// auto serialize(const ShaderTargetInfo & person) -> zpp::bits::members<2>;

constexpr static auto serialize(auto& archive, Hash64City const& self) {
    return archive(uint64_t(*self.hash_code.data()));
}

void GlobalShaderCache::Impl::Load() {
    const auto& cache_root = Moer::ConfigManager::GetInstance().GetEngineShaderCachedPath();
    for (uint32_t index = 0; index < EShaderPlatform::SP_Num; index++) {
        auto& cache = shader_cache[index];
        auto  path  = cache_root / std::format("{}.cache", ToString((EShaderPlatform)index));
        if (!std::filesystem::exists(path)) {
            continue;
        }
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            continue;
        }
        auto*             file_buff = file.rdbuf();
        std::stringstream ss;
        ss << file_buff;
        auto in = zpp::bits::in(ss.rdbuf()->view());
        in(shader_cache[index]).or_throw();
    }
}
void GlobalShaderCache::Impl::Dump() {
    const auto& cache_root = Moer::ConfigManager::GetInstance().GetEngineShaderCachedPath();

    // FunctionGraphTask::ConstructAndDispatchWhenReady([this, cache_root]() {
    for (uint32_t index = 0; index < EShaderPlatform::SP_Num; index++) {
        auto& cache = shader_cache[index];
        if (cache.IsEmpty()) {
            continue;
        }
        auto path = cache_root / std::format("{}.cache", ToString((EShaderPlatform)index));
        if (!std::filesystem::exists(path.parent_path())) {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream file(path, std::ios::binary);
        if (!file.is_open()) {
            continue;
        }
        Moer::Array<uint8_t> cache_stream;
        auto                 out = zpp::bits::out(cache_stream);
        out(cache).or_throw();
        file.write(reinterpret_cast<const char*>(cache_stream.data()), cache_stream.size());
    }
    // });
}

GlobalShaderCache::GlobalShaderCache() {
    impl = new Impl();
}

GlobalShaderCache::~GlobalShaderCache() {
    delete impl;
}

void GlobalShaderCache::Load() {
    impl->Load();
}

void GlobalShaderCache::Dump() {
    impl->Dump();
}

void GlobalShaderCache::UpdateOutput(Moer::Array<ShaderCompilerOutput*>& outputs) {
    impl->UpdateOutput(outputs);
}

const ShaderCompilerOutput* GlobalShaderCache::FindShaderCache(EShaderPlatform platform, const ShaderResourceKey& key) const {
    return impl->FindShaderCache(platform, key);
}

ShaderResourceManager::ShaderResourceManager() {
}

void ShaderResourceManager::Init(EShaderPlatform _platform) {
    auto& t_ptr = GetInstance().type_resources;
    t_ptr       = new ShaderTypeResourceMap(_platform);

    auto& s_ptr = GetInstance().shader_resources;
    s_ptr       = new ShaderResourceMap();
}

void ShaderResourceManager::ShutDown() {
    for (uint32_t index = 0; index < EShaderPlatform::SP_Num; index++) {

        ShaderTypeResourceMap* t_ptr = GetInstance().type_resources;
        if (t_ptr != nullptr) {
            t_ptr->~ShaderTypeResourceMap();
            GetInstance().type_resources = nullptr;
        }

        ShaderResourceMap* s_ptr = GetInstance().shader_resources;
        if (s_ptr != nullptr) {
            s_ptr->~ShaderResourceMap();
            GetInstance().shader_resources = nullptr;
        }
    }
}

ShaderResourceManager& ShaderResourceManager::GetInstance() {
    static ShaderResourceManager manager;
    return manager;
}

static LockFreeQueueBase<ShaderCompilerOutput> g_shader_compile_output_queue;

void ShaderResourceManager::PrepareGlobalShaderResources() {

    Moer::Timer timer;
    timer.Start();
    //submit registrated shader types
    ShaderTypeRegistration::SubmitRegistrations();

    //retrieve all possible compile works
    const auto& works = ShaderCompileRegistration::RetrieveShaderCompileWorks();

    GlobalShaderCache::GetInstance().Load();

    static auto post_process = [this](ShaderCompilerOutput*& output) {
        if (!output->b_succeeded) {
            // ShaderMetaType* meta_type = ShaderMetaType::GetShaderMetaType(output->shader_name_hash);
            std::string error_msg = std::format("Shader compilation failed.");

            std::for_each(output->errors.begin(), output->errors.end(), [&error_msg](const std::string& error) {
                error_msg += error + "\n";
            });

            LOG_ERROR(error_msg);
            MoerDelete(output);
            output = nullptr;
            return;
        }
        auto& resource_map = GetShaderResourceMap();

        //add shader code
        resource_map.AddShaderCompilerOutput(ShaderResourceKey{output->shader_name_hash, output->mutation_id}, *output);

        ShaderMetaType* meta_type = ShaderMetaType::GetShaderMetaType(output->shader_name_hash);

        Shader* shader = meta_type->ConstructShaderInstance(ShaderCompiledInitializer(
            meta_type,
            *output));
        //add shader instance
        GetShaderTypeMap().AddShader(output->shader_name_hash, shader);
    };
    LOG_INFO("Load Shader Cache Time(ms): {}", timer.ElapsedMilliseconds());
    //todo: parallel compiling
    std::for_each(works.begin(), works.end(), [](const ShaderCompileJobInput& input) {
        ShaderResourceManager& self = GetInstance();
        ShaderCompileJob       job;
        job.Finalize(input);
        job.DispatchAndExecute(post_process);

        Moer::Array<ShaderCompilerOutput*> outputs;
        job.ExportOutput(outputs);

        for (auto* output : outputs | std::views::filter([](auto* _output) {
                                if (!_output) {
                                    return false;
                                }
                                if (_output->cached) {
                                    LOG_INFO("Shader {} is cached.", _output->shader_name_hash);
                                    return true;
                                }
                                return true;
                            })) {
            g_shader_compile_output_queue.Push(output);
        }
    });
    LOG_INFO("Process Global Shader Data Time(ms): {}", timer.ElapsedMilliseconds());

    //dump cache bundle, TODO: do it on IO thread

#ifndef _DEBUG
    LambdaTask::Dispatch([this]() {
        Moer::Array<ShaderCompilerOutput*> outputs;
        g_shader_compile_output_queue.PopAll(outputs);
        GlobalShaderCache::GetInstance().UpdateOutput(outputs);

        GlobalShaderCache::GetInstance().Dump();
    });
#endif
}

RHIShaderRef ShaderResourceManager::GetShader(const ShaderMetaType& _meta_type, uint32_t _mutation_id) {
    //sync problem
    assert(type_resources != nullptr);
    Shader* shader = type_resources->FindOrAddShader(_meta_type.GetNameHash(), nullptr);
    if (shader == nullptr) return nullptr;

    ShaderResourceKey key{shader->GetShaderMetaType()->GetNameHash(), _mutation_id};
    // if(_meta_type.GetName())
    return shader_resources->GetRHIShader(key, shader);
}

Shader* ShaderResourceManager::GetShader(const ShaderMetaType& _meta_type) {
    assert(type_resources != nullptr);
    Shader* shader = type_resources->FindOrAddShader(_meta_type.GetNameHash(), nullptr);
    if (shader == nullptr) return nullptr;
    return shader;
}
