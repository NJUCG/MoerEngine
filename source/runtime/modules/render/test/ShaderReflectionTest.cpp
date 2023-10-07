#include "shader/ShaderParameterMacros.h"
#include "shader/ShaderCommon.h"
#include "misc/Hash.h"
BEGIN_SHADER_PARAMETER_DEFINITION(TestParameters)

DEFINE_SHADER_PARAM_UAV(RWBuffer2D, buffer2d)
DEFINE_SHADER_PARAM_SRV(StructuredBuffer, sbo)
DEFINE_SHADER_PARAM_SAMPLER(Sampler, sampler)
DEFINE_SHADER_PARAM_SRV_ARRAY(TextureSRVArray, srv_array, 10)
DEFINE_SHADER_PARAM_ATTACHMENT_BINDING()
DEFINE_SHADER_PARAM(int2, rect)
END_SHADER_PARAMETER_DEFINITION(Parameters)

class TestGlobalShader : public ShaderBase {

public:
    BEGIN_SHADER_PARAMETER_DEFINITION(Parameters)

    DEFINE_SHADER_PARAM_UAV(RWBuffer2D, buffer2d)
    DEFINE_SHADER_PARAM_SRV(StructuredBuffer, sbo)
    DEFINE_SHADER_PARAM_SAMPLER(Sampler, sampler)
    DEFINE_SHADER_PARAM_SRV_ARRAY(TextureSRVArray, srv_array, 10)
    DEFINE_SHADER_PARAM_STRUCT(StructuredBuffer, TestParameters)
    END_SHADER_PARAMETER_DEFINITION(Parameters)
};

void test() {
    TestGlobalShader::Parameters* pass;
    const auto&                   members          = TestGlobalShader::Parameters::GetMembers();
    const auto                    struct_mata_data = TestGlobalShader::Parameters::TypeInfo::GetStructMetadata();
    int                           i                = 1;
}
int main() {
    test();
    return 0;
}