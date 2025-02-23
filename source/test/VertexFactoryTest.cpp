#include <iostream>
#include <type_traits>
#include "resources/vertexfactory/VertexAttributes.h"
#include "resources/vertexfactory/VertexFactoryBuffers.h"
#include "misc/STL.h"
#include "log/LogSystem.h"

using namespace Moer;

void TestVertexAttributes() {
    static_assert(std::is_same_v<VertexAttributesType<EVertexAttributes::VA_POSITION>::type, float3>);
    static_assert(std::is_same_v<VertexAttributesType<EVertexAttributes::VA_NORMAL>::type, uint>);
    static_assert(std::is_same_v<VertexAttributesType<EVertexAttributes::VA_TANGENT>::type, uint>);
    static_assert(std::is_same_v<VertexAttributesType<EVertexAttributes::VA_TEXCOORD0>::type, float2>);

    static_assert(VertexAttributesPixelFormat<EVertexAttributes::VA_POSITION>::PF == PF_R32G32B32_SFLOAT);
    static_assert(VertexAttributesPixelFormat<EVertexAttributes::VA_NORMAL>::PF == PF_R32_UINT);
    static_assert(VertexAttributesPixelFormat<EVertexAttributes::VA_TANGENT>::PF == PF_R32_UINT);
    static_assert(VertexAttributesPixelFormat<EVertexAttributes::VA_TEXCOORD0>::PF == PF_R32G32_SFLOAT);
}

// MARK: Main Function
int main(int argc, const char** argv) {

    LOG_INFO("VertexFactoryTest Start!");

    TestVertexAttributes();

    TestVertexFactoryBuffers();// in VertexFactory.h

    LOG_INFO("VertexFactoryTest Passed!");

    return 0;
}