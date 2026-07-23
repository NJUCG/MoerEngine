#include "rhi/RHIImpl.h"
#include "rhi/vulkan/VulkanParallelRecordWorkEstimator.h"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

using namespace Moer;
using namespace Moer::Render;

namespace {

void Expect(bool _condition, const char* _message) {
    if (!_condition) {
        throw std::runtime_error(_message);
    }
}

class TestBuffer final : public Buffer {
public:
    TestBuffer() : Buffer(BufferInfo{64, 4, EBufferUsageFlags::NONE}) {}

    void SetName(const std::string_view _name) override {
        (void)_name;
    }
};

PipelineHandle MakePipeline() {
    PipelineHandle pipeline;
    // The fourth argument below is deliberately inactive and must not affect
    // the estimate.
    pipeline.valid_bits = (uint64{1} << 0) | (uint64{1} << 1) | (uint64{1} << 2);
    return pipeline;
}

ArrayArguments MakeArguments() {
    ArrayArguments arguments(4, 2, false);
    arguments[0] = BufferView(nullptr, 0, 1, 4);

    TextureViewArray textures;
    for (uint32 index = 0; index < 3; ++index) {
        TextureView texture{};
        texture.texture   = nullptr;
        texture.format    = PF_UNDEFINED;
        texture.num_mips  = 1;
        texture.num_array = 1;
        textures.emplace_back(texture);
    }
    arguments[1] = std::move(textures);
    arguments[2] = TInvalidArg{0};
    arguments[3] = BufferView(nullptr, 0, 128, 4);
    return arguments;
}

MeshDrawData MakeMeshDraw(TestBuffer& _buffer) {
    MeshDrawData mesh;
    mesh.vtx_views.emplace_back(VertexBuffer{&_buffer, 0});
    mesh.idx_view = IndexBuffer{BufferView(&_buffer, 0, 4, 4), IET_UINT32};
    mesh.EmplaceDrawIndexed(0, 3, 0, 0);
    mesh.EmplaceDrawIndexed(3, 3, 0, 0);
    mesh.DrawIndirect(BufferView(&_buffer, 16, 4, 4), 4096, 16);
    return mesh;
}

void SetDrawStateCountsRecorderOperations() {
    TestBuffer    buffer;
    PipelineHandle pipeline = MakePipeline();
    Array<MeshDrawData> meshes;
    meshes.emplace_back(MakeMeshDraw(buffer));

    SetDrawStateCmd command(
        pipeline, MakeArguments(), RenderPassInfo{}, std::move(meshes), "EstimatorSetDraw"
    );
    const TCachedArgArray no_cached_arguments;

    // 8 fixed + (one scalar + three array elements) + push constants
    // + (one vertex bind + one index bind + two direct draws + one indirect draw).
    Expect(
        VulkanParallelRecordDetail::EstimateWorkUnits(command, no_cached_arguments) == 18,
        "SetDrawState work estimate did not match its recorder operation shape"
    );
}

void MultiDrawCountsInlineAndCachedArguments() {
    TestBuffer     buffer;
    PipelineHandle pipeline = MakePipeline();
    TCachedArgArray cached_arguments;
    cached_arguments.emplace_back(MakeArguments());

    DrawBatch batch;
    DrawBatchElement& inline_element = batch.draw_cmds.emplace_back();
    inline_element.handle            = pipeline;
    inline_element.args              = MakeArguments();
    inline_element.RegisterDrawData(MakeMeshDraw(buffer));

    DrawBatchElement& cached_element = batch.draw_cmds.emplace_back();
    cached_element.handle            = pipeline;
    cached_element.args              = ArrayArgReference{0};
    cached_element.RegisterMeshDispatch(DispatchMeshData::Dispatch(uint3{1, 1, 1}));
    cached_element.RegisterMeshDispatch(DispatchMeshData::Dispatch(uint3{128, 64, 32}));

    MultiDrawCmd command(std::move(batch), RenderPassInfo{}, "EstimatorMultiDraw");

    // 6 fixed + first element (2 bind + 4 args + 1 constants + 5 mesh)
    // + second element (2 bind + 4 cached args + 1 constants + 2 mesh dispatches).
    Expect(
        VulkanParallelRecordDetail::EstimateWorkUnits(command, cached_arguments) == 27,
        "MultiDraw work estimate lost inline, cached, draw, or mesh-dispatch work"
    );
}

void DispatchCountsConstantsButNotGpuPayloadSize() {
    TestBuffer     buffer;
    PipelineHandle pipeline = MakePipeline();
    TCachedArgArray cached_arguments;
    cached_arguments.emplace_back(MakeArguments());

    DispatchCmd small(
        TShaderArgArray{MakeArguments()},
        pipeline,
        uint3{1, 1, 1},
        ProfileSection{"Other"},
        "EstimatorDispatchSmall"
    );
    DispatchCmd large(
        TShaderArgArray{MakeArguments()},
        pipeline,
        uint3{65535, 65535, 65535},
        ProfileSection{"Other"},
        "EstimatorDispatchLarge"
    );
    DispatchCmd indirect(
        TShaderArgArray{ArrayArgReference{0}},
        pipeline,
        BufferView(&buffer, 0, 4, 4),
        ProfileSection{"Other"},
        "EstimatorDispatchIndirect"
    );

    const uint32 expected = 10; // 5 fixed + 4 arguments + one push-constant record.
    Expect(
        VulkanParallelRecordDetail::EstimateWorkUnits(small, cached_arguments) == expected,
        "Dispatch estimate did not include push constants"
    );
    Expect(
        VulkanParallelRecordDetail::EstimateWorkUnits(large, cached_arguments) == expected,
        "Dispatch group counts incorrectly changed recorder work"
    );
    Expect(
        VulkanParallelRecordDetail::EstimateWorkUnits(indirect, cached_arguments) == expected,
        "cached indirect Dispatch did not match direct Dispatch recorder work"
    );
}

void CopyWorkIgnoresGpuPayloadVolume() {
    const TCachedArgArray no_cached_arguments;
    CopyBufferCmd small(1, 2, 0, 0, 4, "EstimatorCopySmall");
    CopyBufferCmd large(
        1,
        2,
        0,
        0,
        std::numeric_limits<uint64>::max(),
        "EstimatorCopyLarge"
    );
    CopyTextureCmd texture(
        PF_R8G8B8A8_UNORM,
        1,
        2,
        0,
        0,
        uint3{0, 0, 0},
        uint3{0, 0, 0},
        uint3{4096, 4096, 64},
        "EstimatorCopyTexture"
    );

    Expect(
        VulkanParallelRecordDetail::EstimateWorkUnits(small, no_cached_arguments) == 3,
        "small buffer copy did not use the fixed recorder cost"
    );
    Expect(
        VulkanParallelRecordDetail::EstimateWorkUnits(large, no_cached_arguments) == 3,
        "copy byte count incorrectly changed recorder work"
    );
    Expect(
        VulkanParallelRecordDetail::EstimateWorkUnits(texture, no_cached_arguments) == 3,
        "copy extent incorrectly changed recorder work"
    );
}

} // namespace

int main() {
    try {
        SetDrawStateCountsRecorderOperations();
        MultiDrawCountsInlineAndCachedArguments();
        DispatchCountsConstantsButNotGpuPayloadSize();
        CopyWorkIgnoresGpuPayloadVolume();
        std::cout << "Vulkan parallel-record work estimator tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Vulkan parallel-record work estimator test failed: " << error.what() << '\n';
        return 1;
    }
}
