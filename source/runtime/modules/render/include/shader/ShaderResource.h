#ifndef MOERENGINE_SHADER_RESOURCE_H
#define MOERENGINE_SHADER_RESOURCE_H
#include <cstdint>
#include <assert.h>
#include <memory>
#include <atomic>
#include <misc/CountableRef.h>
#include <misc/Hash.h>
#include <vector>
#include "ShaderCommon.h"

class ShaderResource : Countable{
public:

protected:
    void Destroy() override{
        delete this;
    }
};



struct ShaderEntry{
    std::vector<uint8_t> code;
    EShaderType type;
};
class ShaderCodeResourceMap : ShaderResource{
public:
    void AddShaderCompilerOutput(const ShaderCompilerOutput& _output);
    int32_t GetIndexByHash(const Hash64City& _hash);
    Hash64City resource_hash;
    std::vector<Hash64City> shader_hashes;
    std::vector<ShaderEntry> shader_entries;
};

//for RHI shader creation
class ShaderMapResource : ShaderResource{

};



#endif//MOERENGINE_SHADER_RESOURCE_H
