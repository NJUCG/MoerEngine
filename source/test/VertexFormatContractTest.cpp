#include "resources/vertexfactory/VertexAttributes.h"
#include "rhi/RHIResource.h"
#include "rhi/vulkan/VulkanVertexFormat.h"
#include "shader/ShaderCommon.h"

#include <cstddef>
#include <stdexcept>
#include <type_traits>

static_assert(!std::is_convertible_v<EPixelFormat, EVertexFormat>);
static_assert(!std::is_convertible_v<EVertexFormat, EPixelFormat>);
static_assert(std::is_constructible_v<Moer::Render::VertexElement, EVertexFormat>);
static_assert(!std::is_constructible_v<Moer::Render::VertexElement, EPixelFormat>);
static_assert(std::is_same_v<decltype(VertexAttrb::format), EVertexFormat>);
static_assert(std::is_same_v<decltype(VertexElement::format), EVertexFormat>);
static_assert(!std::is_assignable_v<decltype(VertexElement::format)&, EPixelFormat>);
static_assert(std::is_same_v<decltype(Moer::Render::RaytracingGeometryInfo::vertex_format), EVertexFormat>);
static_assert(
    !std::is_constructible_v<VertexElement, uint8_t, uint8_t, EPixelFormat, uint8_t, uint16_t, EVertexInputRate>
);

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main() {
    using namespace Moer;
    using namespace Moer::Render;

    // ImGui interleaves two float2 attributes and a packed normalized color.
    struct UiVertex {
        float   position[2];
        float   uv[2];
        uint8_t color[4];
    };
    VertexStream ui_stream;
    ui_stream.EmplacePerVertex(
        {{EVertexFormat::Float2}, {EVertexFormat::Float2}, {EVertexFormat::UByte4Normalized}}
    );
    uint32_t stride = 0;
    for (const auto& element : ui_stream.bindings.front().vertex_elements) {
        stride += GetVertexFormatByteSize(element.format);
    }
    Require(stride == sizeof(UiVertex), "Vertex format stride must match the interleaved UI buffer");
    Require(
        GetVertexFormatByteSize(EVertexFormat::Float2) * 2 == offsetof(UiVertex, color),
        "Normalized vertex colors must follow the position and UV data"
    );
    Require(
        ToVulkanVertexFormat(EVertexFormat::UByte4Normalized) == VK_FORMAT_R8G8B8A8_UNORM,
        "Vertex colors must use normalized linear fetch, without an sRGB conversion"
    );
    Require(
        GetVertexFormatByteSize(EVertexFormat::Undefined) == 0 &&
                ToVulkanVertexFormat(EVertexFormat::Undefined) == VK_FORMAT_UNDEFINED,
        "Undefined vertex formats must have no native layout"
    );

    const Array<EVertexAttributes> attributes = {
        EVertexAttributes::VA_POSITION, EVertexAttributes::VA_NORMAL, EVertexAttributes::VA_TEXCOORD0
    };
    VertexFactory       factory(VertexAttributesTool::GetBitmaskFromArray(attributes), false);
    const VertexStream& stream = factory.GetVertexStream();
    Require(stream.bindings.size() == attributes.size(), "Vertex factory must preserve separate bindings");
    for (size_t i = 0; i < attributes.size(); ++i) {
        const EVertexFormat format = VertexAttributesTool::GetVertexFormat(attributes[i]);
        Require(
            stream.bindings[i].vertex_elements.size() == 1 &&
                    stream.bindings[i].vertex_elements.front().format == format,
            "Vertex factory must preserve the attribute format"
        );
        Require(
            GetVertexFormatByteSize(format) == VertexAttributesTool::GetSize(attributes[i]),
            "Vertex storage types must agree with the vertex fetch stride"
        );
    }
    Require(
        stream.bindings[1].vertex_elements.front().format == EVertexFormat::UInt &&
                ToVulkanVertexFormat(EVertexFormat::UInt) == VK_FORMAT_R32_UINT,
        "Packed normals must be fetched as integer data"
    );
    Require(
        RaytracingGeometryInfo{}.vertex_format == EVertexFormat::Float3,
        "Ray tracing geometry must default to float3 positions"
    );
    return 0;
}
