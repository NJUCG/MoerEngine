#ifndef MOERENGINE_SHADER_COMMON_H
#define MOERENGINE_SHADER_COMMON_H
#include "rhi/RHIResource.h"
#include <cstdint>
#include "misc/MacroUtils.h"
enum class EShaderParameterType : uint8_t {
    LOOSE_DATA,
    UNIFORM_BUFFER,
    SAMPLER,
    SRV,
    UAV,

    BINDLESS_RESOURCE_INDEX,
    BINDLESS_SAMPLER_INDEX,

    Num
};

enum class EShaderPrecisionModifier : uint8_t {
    FLOAT,
    HALF,
    FIXED,
    INVALID
};
ENUM_BIT_OP_IMPL(EShaderPrecisionModifier)

//compiled shader platform and type information
struct alignas(4) ShaderTargetInfo {
    EShaderType shader_type : ST_NumBits;
    EShaderPlatform shader_platform : SP_NumBits;
};

typedef uint32_t ShaderTypeIndex;

//contains parameter info
class ShaderTypeInfo {
public:
    struct Parameters {
    };

private:
    //todo: currently only support one file one shader, this field for multiple shader single file
    const char* name;
    std::string type_name;
    uint32_t hash_name;
    uint32_t hash_type_name;
    const char* file_name;
    const char* function_name;
    EShaderType shader_type;
    ShaderTypeIndex type_index;
};

class ShaderPipelineType{
    enum class Type: uint8_t{
        Graphics,
        Mesh
    };
public:
    virtual ~ShaderPipelineType(){}
    ShaderPipelineType(
        const char* _name,
        const ShaderTypeInfo* _vertex_shader_info,
        const ShaderTypeInfo* _geometry_shader_info,
        const ShaderTypeInfo* _fragment_shader_info);

    ShaderPipelineType(
        const char* _name,
        const ShaderTypeInfo* _vertex_shader_info,
        const ShaderTypeInfo* _geometry_shader_info);

    friend uint32_t GetHash(const ShaderPipelineType* _p_value){return _p_value == nullptr ? 0 : _p_value->hash_index;}

    //todo: currently support only global shader
    bool IsGlobalShaderPipeline()const{return shader_stages[0];}
    const Hash64City& GetSourceCodehash(EShaderPlatform _platform) const;

    const char* GetName() const { return name; }
    Hash64City hash_name;
    Hash64City hash_file_name;

    std::array<ShaderTypeInfo*, ST_Num> shader_stages;

    uint32_t hash_index;
    bool b_initialized;
    const char* name;
};
// per parameter allocation in global map
struct ShaderParameterAllocationInfo {
    //global buffer index
    uint16_t             buffer_index = 0;
    //binding/slot
    uint16_t             slot         = 0;
    uint16_t             size         = 0;
    EShaderParameterType type{EShaderParameterType::Num};
    mutable bool         b_bound = false;

    ShaderParameterAllocationInfo() = default;
    ShaderParameterAllocationInfo(
        uint16_t             _buffer_index,
        uint16_t             _slot,
        uint16_t             _size,
        EShaderParameterType _type) : buffer_index(_buffer_index),
                                      slot(_slot),
                                      size(_size),
                                      type(_type) {
    }
};

class ShaderParameterMap {
public:
    ShaderParameterMap() = default;

    std::optional<ShaderParameterAllocationInfo> FindShaderParameterAllocation(const std::string& _param_name) const;
    void                                         AddShaderParameterAllocation(const char* _param_name, uint16_t _buffer_index, uint16_t _base_index, uint16_t _size, EShaderParameterType _type);
    void                                         RemoveShaderParameterAllocation(const char* _param_name);

    inline void GetAllParamsNames(std::vector<std::string>& _out_names) const {
        for (const auto& params_pair : shader_parameters_map) {
            _out_names.push_back(params_pair.first);
        }
    }
    inline const std::unordered_map<std::string, ShaderParameterAllocationInfo>& GetShaderParameterMap() const {
        return shader_parameters_map;
    }

private:
    std::unordered_map<std::string, ShaderParameterAllocationInfo> shader_parameters_map;
};

struct ShaderCompilerOutput{

    ShaderCompilerOutput()
    : num_instructions(0),
    num_samplers(0),
    compiled_time(0.0),
    preprocessing_time(0.0),
    b_succeeded(false){}

    ShaderParameterMap parameter_map;
    std::vector<std::string> errors;
    std::vector<std::string> pragma;

    ShaderTargetInfo target_info;

    std::vector<uint8_t> shader_code;
    Hash64City           compiled_hash;
    uint32_t num_instructions;
    uint32_t num_samplers;
    double compiled_time;
    double preprocessing_time;
    bool b_succeeded;

    ShaderCompilerOutput(ShaderCompilerOutput&&) = default;
    ShaderCompilerOutput(const ShaderCompilerOutput&) = default;
    ShaderCompilerOutput& operator=(ShaderCompilerOutput&&) = default;
    ShaderCompilerOutput& operator=(const ShaderCompilerOutput&) = default;

    void GenerateCompiledHash();
};


struct ShaderCompiledInfo {
    const ShaderTypeInfo*       type_info;
    ShaderTargetInfo            target_info;
    const std::vector<uint8_t>& compiled_code;
    const ShaderParameterMap&   ParameterMap;
    const Hash64City&           OutputHash;
    Hash64City                  MaterialShaderMapHash;
    const ShaderPipelineType*   ShaderPipeline;
    //    const VertexFactoryType* VertexFactoryType;
    uint32_t NumInstructions;
    uint32_t NumTextureSamplers;
    uint32_t CodeSize;
    int32_t  PermutationId;

    RENDER_CORE_API ShaderCompiledInfo(
        const ShaderTypeInfo*           _shader_type,
        const ShaderCompilerOutput& _compiled_output,
        const Hash64City&           _material_shader_map_hash,
        const ShaderPipelineType*   _shader_pipeline_type
        //        const FVertexFactoryType* InVertexFactoryType
    );


};
#endif//MOERENGINE_SHADER_COMMON_H
