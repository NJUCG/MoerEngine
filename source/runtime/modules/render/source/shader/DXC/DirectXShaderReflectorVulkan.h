#include "../ShaderReflector.h"

class DirectXShaderReflectorVulkan : public ShaderReflector {
public:
    DirectXShaderReflectorVulkan() {
    }

    // Destructor
    virtual ~DirectXShaderReflectorVulkan() {
        // Clean up any dynamically allocated memory or resources
    }

    void ReflectShader(const void* _compiled_result, const ShaderParametersMetadata* _param_meta_data, std::unordered_map<std::string, ParameterInfo>& _out_parameters) override;
};