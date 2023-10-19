#include "shader/Shader.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderParameterMacros.h"
#include "shader/ShaderCommon.h"
#include "misc/Hash.h"
#include "log/LogSystem.h"
BEGIN_SHADER_UNIFORM_STRUCT_DEFINITION(TestParameters)

DEFINE_SHADER_PARAM(Moer::Vector2i, rect)
END_SHADER_UNIFORM_STRUCT_DEFINITION(Parameters)

class TestGlobalShader : public Shader {

public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)

    DEFINE_SHADER_PARAM_UAV(RWBuffer2D, buffer2d)
    DEFINE_SHADER_PARAM_SRV(StructuredBuffer, sbo)
    DEFINE_SHADER_PARAM_SAMPLER(Sampler, sampler)
    DEFINE_SHADER_PARAM_SRV_ARRAY(TextureSRVArray, srv_array, 10)

    END_ROOT_PARAMETER_DEFINITION(Parameters)
};

void test() {
    TestGlobalShader::Parameters* pass;
    const auto&                   members          = TestGlobalShader::Parameters::GetMembers();
    const auto                    struct_mata_data = TestGlobalShader::Parameters::TypeInfo::GetStructMetadata();
    int                           i                = 1;
}
int main() {
    test();
    ShaderCompiler::ShaderConductorTest();
    return 0;
}