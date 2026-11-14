#include "config/ConfigManager.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIImpl.h"
#include "rhi/backends/d3d12/D3D12Device.h"
#include "rhi/executor/RHIExecutor.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/test/D3D12ConstantPacking.h"
#include "shaderheaders/shared/test/ShaderConstants.h"
#include "taskgraph/TaskSystem.h"
#include <GLFW/glfw3.h>
#include <array>
#include <cstring>
#include <iostream>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

extern "C" {
__declspec(dllexport) extern const unsigned int D3D12SDKVersion = MOER_AGILITY_SDK_VERSION;
__declspec(dllexport) extern const char*        D3D12SDKPath    = ".\\D3D12\\";
}

namespace {
using namespace Moer;
using namespace Moer::Render;

void Require(bool value, const char* reason) {
    if (!value)
        throw std::runtime_error(reason);
}
template<typename T>
std::span<Moer::byte> Bytes(T& value) {
    return {reinterpret_cast<Moer::byte*>(value.data()), value.size() * sizeof(value[0])};
}
void Submit(CommandList& list, EQueueType type = EQueueType::Graphics) {
    RHIExecutor::Get().Submit(type, list.Submit());
    RHIExecutor::Get().Sync();
}
ReadbackResult Result(ReadbackFuture future) {
    Require(future.Valid(), "Owning readback unsupported");
    Require(future.WaitFor(std::chrono::seconds(10)), "GPU readback timed out");
    const auto result = future.Get();
    Require(result.status == ReadbackStatus::Ready, result.error_reason.c_str());
    return result;
}
class Constants : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(Constants);
    DEFINE_SHADER_BUFFER(values);
    DEFINE_SHADER_CONSTANTS(SHADER_TEST_CONSTANTS)
    DEFINE_SHADER_ARGS(scale, values, data, tint, transform, bias);
};
class BindlessCompute : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(BindlessCompute);
    DEFINE_SHADER_BUFFER(output);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT(uint4, params);
    DEFINE_SHADER_ARGS(output, bdls, params);
};
class RayCompute : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(RayCompute);
    DEFINE_SHADER_BUFFER(output);
    DEFINE_SHADER_TLAS(scene);
    DEFINE_SHADER_ARGS(output, scene);
};
class PackingCompute : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(PackingCompute);
    DEFINE_SHADER_CONSTANT(D3D12PackingData, packed);
    DEFINE_SHADER_BUFFER(packed_buffer);
    DEFINE_SHADER_BUFFER(values);
    DEFINE_SHADER_ARGS(packed, packed_buffer, values);
};
class TypedCompute : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(TypedCompute);
    DEFINE_SHADER_BUFFER(typed_input);
    DEFINE_SHADER_BUFFER(typed_output);
    DEFINE_SHADER_ARGS(typed_input, typed_output);
};
class StructuredCompute : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(StructuredCompute);
    DEFINE_SHADER_BUFFER(structured_input);
    DEFINE_SHADER_BUFFER(structured_output);
    DEFINE_SHADER_ARGS(structured_input, structured_output);
};
class ArrayCompute : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(ArrayCompute);
    DEFINE_SHADER_BUFFER_ARRAY(structured_inputs, 3);
    DEFINE_SHADER_TEX_ARRAY(array_outputs, 3);
    DEFINE_SHADER_ARGS(structured_inputs, array_outputs);
};
class Triangle : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(Triangle);
    DEFINE_SHADER_ARGS();
};
class DrawOffsets : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(DrawOffsets);
    DEFINE_SHADER_BUFFER(output);
    DEFINE_SHADER_ARGS(output);
};

void Transfers() {
    auto&                  device = RenderDevice::Get();
    auto                   a      = device.CreateBuffer<uint32>("source", 32, EBufferUsageFlags::NONE);
    auto                   b      = device.CreateBuffer<uint32>("destination", 32, EBufferUsageFlags::UNORDERED_ACCESS);
    std::array<uint32, 32> data{};
    for (uint32 i = 0; i < data.size(); ++i)
        data[i] = i * 17 + 3;
    CommandList upload(EQueueType::Copy);
    upload.CopyFrom(Bytes(data), a->GetView());
    upload.CopyFrom(a->GetView(), b->GetView());
    auto future    = upload.Readback(b->GetView());
    auto timestamp = upload.BeginTimestampQuery("copy timestamp");
    upload.EndTimestampQuery(timestamp);
    Submit(upload, EQueueType::Copy);
    const auto result = Result(future);
    Require(result.ByteSize() == sizeof(data) && std::memcmp(result.Bytes().data(), data.data(), sizeof(data)) == 0, "Buffer transfer mismatch");
    Require(timestamp.GetFuture().Get().status == QueryStatus::Ready, "Copy timestamp query failed");
    for (const auto format : {PF_R8_UNORM, PF_R8G8B8A8_UNORM, PF_R16G16B16A16_SFLOAT, PF_BC1_RGBA_UNORM_BLOCK}) {
        auto              texture = device.CreateTexture(Extent2D(7, 5), format, ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_SRC);
        Array<Moer::byte> pixels(texture->GetMipByteSize(0));
        for (uint32 i = 0; i < pixels.size(); ++i)
            pixels[i] = Moer::byte(i * 7);
        auto        tight = device.CreateBuffer<Moer::byte>("tight copy", uint32(pixels.size()), EBufferUsageFlags::NONE);
        CommandList commands;
        commands.CopyFrom(std::span<Moer::byte>(pixels), texture->GetView());
        commands.CopyFrom(texture->GetView(), tight->GetView());
        auto read         = commands.Readback(tight->GetView());
        auto texture_read = commands.Readback(texture->GetView());
        Submit(commands);
        const auto copied = Result(read);
        Require(copied.ByteSize() == pixels.size() && std::memcmp(copied.Bytes().data(), pixels.data(), pixels.size()) == 0, "Pitched texture transfer mismatch");
        if (texture_read.Valid()) {
            const auto copied_texture = Result(texture_read);
            Require(copied_texture.ByteSize() == pixels.size() && std::memcmp(copied_texture.Bytes().data(), pixels.data(), pixels.size()) == 0, "Owning texture readback mismatch");
        }
        CommandList reverse;
        reverse.CopyFrom(tight->GetView(), texture->GetView());
        auto reverse_read = reverse.Readback(texture->GetView());
        Submit(reverse);
        if (reverse_read.Valid())
            Require(std::memcmp(Result(reverse_read).Bytes().data(), pixels.data(), pixels.size()) == 0, "Buffer-to-texture row packing mismatch");
    }
    std::cout << "buffer, odd-width texture, BC and Copy queue roundtrips passed\n";
}
void Compute() {
    {
        struct Element {
            float3 value;
            uint32 key;
            uint32 weight;
        };
        auto                   pipeline = ShaderManager::Get().Compute<StructuredCompute>("test/D3D12Backend.hlsl", "StructuredMain");
        std::array<Element, 3> data{{{{1, 2, 3}, 7, 9}, {{4, 5, 6}, 17, 19}, {{7, 8, 9}, 27, 29}}};
        auto                   input  = RenderDevice::Get().CreateBuffer<Moer::byte>("raw allocation structured input", sizeof(data), EBufferUsageFlags::NONE);
        auto                   output = RenderDevice::Get().CreateBuffer<Moer::byte>("raw allocation structured output", sizeof(data), EBufferUsageFlags::UNORDERED_ACCESS);
        CommandList            commands;
        commands.CopyFrom(Bytes(data), input->GetView());
        commands.Compute(pipeline, input, output).Dispatch(3);
        auto read = commands.Readback(output->GetView());
        Submit(commands);
        for (auto& value : data)
            value.key += 100;
        Require(std::memcmp(Result(read).Bytes().data(), data.data(), sizeof(data)) == 0, "Structured descriptor stride must follow DXIL, not allocation stride");
        auto                          arrays = ShaderManager::Get().Compute<ArrayCompute>("test/D3D12Backend.hlsl", "ArrayMain");
        std::array<TextureRef, 3>     textures;
        std::array<TextureView, 3>    texture_views;
        std::array<BufferView, 3>     buffer_views;
        std::array<ReadbackFuture, 3> reads;
        CommandList                   array_commands;
        for (uint32 i = 0; i < 3; ++i) {
            textures[i]      = RenderDevice::Get().CreateTexture(Extent2D(1, 1), PF_R32_UINT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::TRANSFER_SRC);
            texture_views[i] = textures[i]->GetView();
            buffer_views[i]  = BufferView(output.Get(), i * sizeof(Element), sizeof(Element), 1);
        }
        array_commands.Compute(arrays, std::span<BufferView>(buffer_views), std::span<TextureView>(texture_views)).Dispatch(3);
        for (uint32 i = 0; i < 3; ++i)
            reads[i] = array_commands.Readback(texture_views[i]);
        Submit(array_commands);
        for (uint32 i = 0; i < 3; ++i)
            Require(Result(reads[i]).ReadValue<uint32>() == data[i].key, "Unbounded buffer/texture descriptor array indexing mismatch");
    }
    {
        auto             packing = ShaderManager::Get().Compute<PackingCompute>("test/D3D12ConstantPacking.hlsl");
        auto             input   = RenderDevice::Get().CreateBuffer("packed constant buffer", BufferInfo{1, sizeof(D3D12PackingData), EBufferUsageFlags::CONSTANT_BUFFER});
        auto             output  = RenderDevice::Get().CreateBuffer<uint32>("packing results", 2, EBufferUsageFlags::UNORDERED_ACCESS);
        D3D12PackingData data{1, {1, 2, 5}, {1, 17, 3}, float4x4::Identity(), {2, 3, 13}};
        data.transform.r0.y = 11;
        std::array<D3D12PackingData, 1> buffer_data{data};
        buffer_data[0].index = 9;
        CommandList commands;
        commands.CopyFrom(Bytes(buffer_data), input->GetView());
        commands.Compute(packing, data, input, output).Dispatch(1);
        auto read = commands.Readback(output->GetView());
        Submit(commands);
        Require(Result(read).CopyAs<uint32>() == Array<uint32>({47, 55}), "Nested float3/matrix/array constant packing mismatch");
    }
    {
        auto                  typed  = ShaderManager::Get().Compute<TypedCompute>("test/D3D12Backend.hlsl", "TypedMain");
        auto                  input  = RenderDevice::Get().CreateBuffer<float2>("typed float2 input", 1, EBufferUsageFlags::NONE);
        auto                  output = RenderDevice::Get().CreateBuffer<uint32>("typed uint output", 1, EBufferUsageFlags::UNORDERED_ACCESS);
        std::array<float2, 1> value{float2{9, 17}};
        CommandList           commands;
        commands.CopyFrom(Bytes(value), input->GetView());
        commands.Compute(typed, input, output).Dispatch(1);
        auto read = commands.Readback(output->GetView());
        Submit(commands);
        Require(Result(read).ReadValue<uint32>() == 26, "DXIL typed buffer format inference failed");
    }
    auto&       manager   = ShaderManager::Get();
    auto&       device    = RenderDevice::Get();
    auto        constants = manager.Compute<Constants>("test/ShaderConstants.hlsl", "ComputeMain");
    auto        output    = device.CreateBuffer<float4>("constants", 1, EBufferUsageFlags::UNORDERED_ACCESS);
    CommandList commands(EQueueType::Compute);
    commands.Compute(constants, 2.0f, output, ConstantTestData{3, 4}, float3{.25f, .5f, .75f}, float4x4::Identity(), 5u).Dispatch(1);
    auto read = commands.Readback(output->GetView());
    Submit(commands, EQueueType::Compute);
    const float4 expected{10, .25f, .75f, 5};
    Require(std::memcmp(Result(read).Bytes().data(), &expected, sizeof(expected)) == 0, "Multiple DXIL constants mismatch");
    auto                  input    = device.CreateBuffer<uint32>("bindless source", 4, EBufferUsageFlags::NONE);
    auto                  values   = device.CreateBuffer<uint32>("bindless output", 4, EBufferUsageFlags::UNORDERED_ACCESS);
    auto                  bindless = device.CreateBindlessArray(64);
    const uint32          handle   = bindless->AllocateBuffer(input->GetView());
    auto                  compute  = manager.Compute<BindlessCompute>("test/D3D12Backend.hlsl", "BindlessMain");
    std::array<uint32, 4> data{1, 2, 3, 4};
    CommandList           bind;
    bind.CopyFrom(Bytes(data), input->GetView());
    bind.UpdateBindlessArray(bindless);
    bind.Compute(compute, values, bindless, uint4{handle, 7, 0, 0}).Dispatch(4);
    auto result = bind.Readback(values->GetView());
    Submit(bind);
    const auto actual = Result(result).CopyAs<uint32>();
    if (actual) {
        std::cout << "bindless actual:";
        for (auto value : *actual)
            std::cout << ' ' << value;
        std::cout << '\n';
    }
    Require(actual && *actual == Array<uint32>({8, 9, 10, 11}), "Bindless compute mismatch");
    CommandList empty_update;
    empty_update.UpdateBindlessArray(bindless);
    Require(empty_update.Submit().cmds.empty(), "Empty bindless update inserted a null command");
    std::cout << "multiple constants, compute, bindless and readback futures passed\n";
}
void SubresourcesAndClears() {
    auto& device       = RenderDevice::Get();
    auto  array        = device.CreateTexture(Extent2D(9, 7), PF_R8G8B8A8_UNORM, ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_SRC, 3, 3);
    auto  volume       = device.CreateTexture(Extent3D(7, 5, 3), PF_R8G8B8A8_UNORM, ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_SRC);
    auto  copied_array = device.CreateTexture(Extent2D(9, 7), PF_R8G8B8A8_UNORM, ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_SRC, 3, 3);
    {
        CommandList                   copy_layers;
        std::array<ReadbackFuture, 2> reads;
        for (uint32 i = 0; i < 2; ++i) {
            Array<Moer::byte> pixels(array->GetMipByteSize(1), Moer::byte(71 + i));
            copy_layers.CopyFrom(std::span<Moer::byte>(pixels), array->GetView(1).Slice(i + 1));
        }
        copy_layers.CopyFrom(array->GetView(1).Slice(1, 2), copied_array->GetView(1).Slice(0, 2));
        for (uint32 i = 0; i < 2; ++i)
            reads[i] = copy_layers.Readback(copied_array->GetView(1).Slice(i));
        Submit(copy_layers);
        for (uint32 i = 0; i < 2; ++i) {
            const auto result = Result(reads[i]);
            Require(result.ByteSize() == array->GetMipByteSize(1) && std::all_of(result.Bytes().begin(), result.Bytes().end(), [i](auto value) {
                        return value == Moer::byte(71 + i);
                    }),
                    "Texture copy lost mip extent or source/destination array ranges");
        }
    }
    for (auto view : {array->GetView(1).Slice(2), volume->GetView()}) {
        Array<Moer::byte> bytes(view.texture->GetMipByteSize(view.mip_level));
        for (uint32 i = 0; i < bytes.size(); ++i)
            bytes[i] = Moer::byte(i * 13 + 7);
        CommandList commands;
        commands.CopyFrom(std::span<Moer::byte>(bytes), view);
        auto read = commands.Readback(view);
        Submit(commands);
        const auto actual = Result(read);
        Require(actual.ByteSize() == bytes.size() && std::memcmp(actual.Bytes().data(), bytes.data(), bytes.size()) == 0, "Mip/array/3D transfer mismatch");
    }
    auto        hdr     = device.CreateTexture(Extent2D(7, 5), PF_R16G16B16A16_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::TRANSFER_SRC);
    auto        integer = device.CreateTexture(Extent2D(7, 5), PF_R32_UINT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::TRANSFER_SRC);
    auto        depth   = device.CreateTexture(Extent2D(7, 5), PF_D32_SFLOAT, ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT | ETextureUsageFlags::TRANSFER_SRC);
    CommandList clear;
    clear.ClearResource(hdr->GetView(), float4{.25f, .5f, .75f, 1});
    clear.ClearResource(integer->GetView(), 123u);
    clear.ClearResource(depth->GetView(), float4{.5f, 0, 0, 0});
    auto hdr_read   = clear.Readback(hdr->GetView());
    auto int_read   = clear.Readback(integer->GetView());
    auto depth_read = clear.Readback(depth->GetView());
    Submit(clear);
    const std::array<uint16, 4> expected{0x3400, 0x3800, 0x3a00, 0x3c00};
    Require(std::memcmp(Result(hdr_read).Bytes().data(), expected.data(), sizeof(expected)) == 0, "Float16 UAV clear mismatch");
    Require(Result(int_read).ReadValue<uint32>() == 123, "Integer UAV clear mismatch");
    if (depth_read.Valid())
        Require(Result(depth_read).ReadValue<float>() == .5f, "Depth clear/readback mismatch");
    std::cout << "mip, array, 3D, integer/float UAV and depth clear passed\n";
}
void Raster() {
    auto&               device = RenderDevice::Get();
    GfxPsoCreateInfo    info(RHIRasterizeInfo::Preset<Rast::CULL_NONE>(), VertexStream{}, {RHIColorAttachmentInfo::Preset<>(PF_R8G8B8A8_UNORM)});
    auto                pipeline = ShaderManager::Get().Raster().Vertex("test/D3D12Backend.hlsl", "VertexMain").Pixel("test/D3D12Backend.hlsl", "PixelMain").Build<Triangle>(std::move(info));
    auto                texture  = device.CreateTexture(Extent2D(7, 5), PF_R8G8B8A8_UNORM, ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::TRANSFER_SRC);
    CommandList         commands;
    const auto          timestamp = commands.BeginTimestampQuery("draw timestamp");
    const auto          occlusion = commands.BeginOcclusionQuery("draw occlusion");
    Array<MeshDrawData> meshes;
    meshes.emplace_back().EmplaceDraw(3, 0, 0);
    commands.Gfx(pipeline).Draw(Rect2D(0, 0, 7, 5), std::move(meshes), ColorAttachment{.target = texture.Get()});
    commands.EndOcclusionQuery(occlusion);
    commands.EndTimestampQuery(timestamp);
    auto read = commands.Readback(texture->GetView());
    Submit(commands);
    const auto  pixels = Result(read);
    const auto* pixel  = reinterpret_cast<const uint8*>(pixels.Bytes().data());
    std::cout << "raster actual: " << uint32(pixel[0]) << ' ' << uint32(pixel[1]) << ' ' << uint32(pixel[2]) << ' ' << uint32(pixel[3]) << '\n';
    Require(std::abs(int32(pixel[0]) - 64) <= 1 && std::abs(int32(pixel[1]) - 128) <= 1 && std::abs(int32(pixel[2]) - 191) <= 1 && pixel[3] == 255, "Raster color mismatch");
    const auto query = occlusion.GetFuture().Get();
    Require(query.status == QueryStatus::Ready && std::get<OcclusionQueryResult>(query.payload).visible, "Occlusion query did not see triangle");
    Require(timestamp.GetFuture().Get().status == QueryStatus::Ready, "Raster timestamp query failed");
    GfxPsoCreateInfo    geometry_info(RHIRasterizeInfo::Preset<Rast::CULL_NONE>(), VertexStream{}, {RHIColorAttachmentInfo::Preset<>(PF_R8G8B8A8_UNORM)});
    auto                geometry = ShaderManager::Get().Raster().Vertex("test/D3D12Backend.hlsl", "VertexMain").Geometry("test/D3D12Backend.hlsl", "GeometryMain").Pixel("test/D3D12Backend.hlsl", "PixelMain").Build<Triangle>(std::move(geometry_info));
    CommandList         geometry_commands;
    Array<MeshDrawData> geometry_draws;
    geometry_draws.emplace_back().EmplaceDraw(3, 0, 0);
    geometry_commands.Gfx(geometry).Draw(Rect2D(0, 0, 7, 5), std::move(geometry_draws), ColorAttachment{.target = texture.Get()});
    auto geometry_read = geometry_commands.Readback(texture->GetView());
    Submit(geometry_commands);
    Require(std::memcmp(Result(geometry_read).Bytes().data(), pixels.Bytes().data(), pixels.ByteSize()) == 0, "Geometry shader raster output mismatch");
    Triangle mesh_pipeline;
    try {
        GfxPsoCreateInfo mesh_info(RHIRasterizeInfo::Preset<Rast::CULL_NONE>(), VertexStream{}, {RHIColorAttachmentInfo::Preset<>(PF_R8G8B8A8_UNORM)});
        mesh_pipeline = ShaderManager::Get().Raster().Mesh("test/D3D12Backend.hlsl", "MeshMain").Pixel("test/D3D12Backend.hlsl", "PixelMain").Build<Triangle>(std::move(mesh_info));
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()) != "Adapter does not support mesh shaders")
            throw;
        std::cout << "Mesh shaders unavailable on adapter\n";
    }
    if (mesh_pipeline.handle.IsValid()) {
        auto                  indirect_mesh = device.CreateBuffer<uint32>("mesh indirect arguments", 3, EBufferUsageFlags::INDIRECT_BUFFER);
        std::array<uint32, 3> groups{1, 1, 1};
        for (bool use_indirect : {false, true}) {
            CommandList mesh_commands;
            mesh_commands.CopyFrom(Bytes(groups), indirect_mesh->GetView());
            DrawBatch batch;
            auto&     draw = batch.Emplace<Triangle>(mesh_pipeline.handle);
            draw.RegisterMeshDispatch(use_indirect ? DispatchMeshData::DispatchIndirect(indirect_mesh->GetView(), 1, 12) : DispatchMeshData::Dispatch({1, 1, 1}));
            mesh_commands.Gfx("mesh shader", Rect2D(0, 0, 7, 5), ColorAttachment{.target = texture.Get()}).AcceptDrawBatch(std::move(batch)).Dispatch();
            auto mesh_read = mesh_commands.Readback(texture->GetView());
            Submit(mesh_commands);
            Require(std::memcmp(Result(mesh_read).Bytes().data(), pixels.Bytes().data(), pixels.ByteSize()) == 0, "Mesh shader direct/indirect raster output mismatch");
        }
        GfxPsoCreateInfo task_info(RHIRasterizeInfo::Preset<Rast::CULL_NONE>(), VertexStream{}, {RHIColorAttachmentInfo::Preset<>(PF_R8G8B8A8_UNORM)});
        auto             task_pipeline = ShaderManager::Get().Raster().Task("test/D3D12Backend.hlsl", "TaskMain").Mesh("test/D3D12Backend.hlsl", "TaskMeshMain").Pixel("test/D3D12Backend.hlsl", "PixelMain").Build<Triangle>(std::move(task_info));
        CommandList      task_commands;
        DrawBatch        task_batch;
        task_batch.Emplace<Triangle>(task_pipeline.handle).RegisterMeshDispatch(DispatchMeshData::Dispatch({1, 1, 1}));
        task_commands.Gfx("amplification shader", Rect2D(0, 0, 7, 5), ColorAttachment{.target = texture.Get()}).AcceptDrawBatch(std::move(task_batch)).Dispatch();
        auto task_read = task_commands.Readback(texture->GetView());
        Submit(task_commands);
        Require(std::memcmp(Result(task_read).Bytes().data(), pixels.Bytes().data(), pixels.ByteSize()) == 0, "Amplification shader payload dispatch mismatch");
    }
    GfxPsoCreateInfo      offset_info(RHIRasterizeInfo::Preset<Rast::CULL_NONE>(), VertexStream{}, {RHIColorAttachmentInfo::Preset<>(PF_R8G8B8A8_UNORM)});
    auto                  offset_pipeline = ShaderManager::Get().Raster().Vertex("test/D3D12Backend.hlsl", "OffsetVertexMain").Pixel("test/D3D12Backend.hlsl", "PixelMain").Build<DrawOffsets>(std::move(offset_info));
    auto                  vertices        = device.CreateBuffer<uint32>("offset results", 2, EBufferUsageFlags::UNORDERED_ACCESS);
    auto                  indices         = device.CreateBuffer<uint32>("index buffer", 3, EBufferUsageFlags::INDEX_BUFFER);
    auto                  indirect        = device.CreateBuffer<uint32>("indexed arguments", 5, EBufferUsageFlags::INDIRECT_BUFFER);
    auto                  count           = device.CreateBuffer<uint32>("indexed count", 1, EBufferUsageFlags::INDIRECT_BUFFER);
    std::array<uint32, 3> index_data{0, 1, 2};
    std::array<uint32, 5> draw{3, 1, 0, 7, 11};
    std::array<uint32, 1> one{1};
    for (bool use_indirect : {false, true}) {
        CommandList offsets;
        offsets.CopyFrom(Bytes(index_data), indices->GetView());
        offsets.CopyFrom(Bytes(draw), indirect->GetView());
        offsets.CopyFrom(Bytes(one), count->GetView());
        Array<MeshDrawData> draws;
        auto&               mesh = draws.emplace_back(Array<VertexBuffer>{}, IndexBuffer{indices->GetView(), IET_UINT32});
        if (use_indirect)
            mesh.DrawIndirect(indirect->GetView(), count->GetView(), 1, 20);
        else
            mesh.EmplaceDrawIndexed(0, 3, 7, 11);
        offsets.Gfx(offset_pipeline, vertices).Draw(Rect2D(0, 0, 7, 5), std::move(draws), ColorAttachment{.target = texture.Get()});
        auto values = offsets.Readback(vertices->GetView());
        Submit(offsets);
        Require(Result(values).CopyAs<uint32>() == Array<uint32>({7, 11}), "Nonzero base vertex/instance draw indexing mismatch");
    }
    std::cout << "raster draw, timestamp and occlusion passed\n";
}
void Multiview() {
    auto& device = RenderDevice::Get();
    if (!device.SupportsMultiview(4)) {
        std::cout << "View instancing unavailable on adapter\n";
        return;
    }
    Require(!device.SupportsMultiview(6), "D3D12 six-view point shadows must keep the serial fallback");
    auto texture = device.CreateTexture(Extent2D(7, 5), PF_R8G8B8A8_UNORM, ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::TRANSFER_SRC, 1, 5);
    for (uint32 mask : {5u, 15u}) {
        GfxPsoCreateInfo info(RHIRasterizeInfo::Preset<Rast::CULL_NONE>(), VertexStream{}, {RHIColorAttachmentInfo::Preset<>(PF_R8G8B8A8_UNORM)});
        info.SetViewMask(mask);
        auto        pipeline = ShaderManager::Get().Raster().Vertex("test/MultiviewLayer.hlsl", "MultiviewVS").Pixel("test/MultiviewLayer.hlsl", "MultiviewPS").Build<Triangle>(std::move(info));
        CommandList commands;
        commands.ClearResource(texture->GetView(0, 1), float4{0, 0, 0, 0});
        Array<MeshDrawData> meshes;
        meshes.emplace_back().EmplaceDraw(3, 0, 0);
        commands.Gfx(pipeline).SetViewMask(mask).Draw(Rect2D(0, 0, 7, 5), std::move(meshes), ColorAttachment{.target = texture.Get(), .action = AC_LOAD_STORE, .array_layer = 1, .array_count = 4});
        std::array<ReadbackFuture, 5> reads;
        for (uint32 layer = 0; layer < 5; ++layer)
            reads[layer] = commands.Readback(texture->GetView(0, 1).Slice(layer));
        Submit(commands);
        const std::array<uint32, 4> colors{0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xff00ffffu};
        for (uint32 layer = 0; layer < 5; ++layer) {
            const uint32 expected = layer && (mask & (1u << (layer - 1))) ? colors[layer - 1] : 0;
            Require(Result(reads[layer]).ReadValue<uint32>() == expected, "Multiview masks or attachment-relative layer offsets mismatch");
        }
    }
    std::cout << "four-view instancing, sparse masks and nonzero attachment base layer passed\n";
}
void Raytracing() {
    auto& device = RenderDevice::Get();
    if (!device.SupportsRaytracingAccelerationStructures()) {
        std::cout << "DXR unavailable on adapter\n";
        return;
    }
    auto                   vertices = device.CreateBuffer<float3>("triangle", 3, EBufferUsageFlags::NONE);
    std::array<float3, 3>  data{float3{-1, -1, 1}, float3{1, -1, 1}, float3{0, 1, 1}};
    RaytracingGeometryInfo info{};
    info.build_flags = ERayTracingAccelerationStructureBuildFlags::ALLOW_UPDATE;
    info.segments.push_back(RaytracingSegment{.vertex_count = 3, .vertex_stride = 12, .primitive_count = 1, .vertex_buffer = vertices});
    auto geometry = device.CreateRaytracingGeometry(info);
    auto scene    = device.CreateRaytracingScene();
    Require(!scene->GetPrevTlas(), "Uninitialized TLAS history must be absent so the renderer can use current TLAS");
    auto& instance       = scene->AddInstance();
    instance.geom        = geometry.Get();
    instance.transform   = Matrix3x4f(float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0});
    auto        pipeline = ShaderManager::Get().Compute<RayCompute>("test/D3D12Backend.hlsl", "RayMain");
    auto        output   = device.CreateBuffer<uint32>("ray hit", 1, EBufferUsageFlags::UNORDERED_ACCESS);
    CommandList commands;
    commands.CopyFrom(Bytes(data), vertices->GetView());
    commands.BuildAccelerationStructures({AccelerationStructureBuildParam{geometry, ERaytracingBuildMode::BUILD}});
    commands.UpdateRaytracingScene(scene);
    commands.Compute(pipeline, output, scene->GetTlas()).Dispatch(1);
    auto read = commands.Readback(output->GetView());
    Submit(commands);
    Require(Result(read).ReadValue<uint32>() == 1, "DXR RayQuery missed triangle");
    const auto old_tlas = scene->GetTlas();
    scene->AdvanceFrame();
    instance.transform.r2.w = 20;
    scene->MarkModified(instance.array_idx);
    CommandList update;
    update.UpdateRaytracingScene(scene);
    Require(scene->GetTlas().Get() != old_tlas.Get() && scene->GetPrevTlas().Get() == old_tlas.Get(), "TLAS generation identity was overwritten");
    update.Compute(pipeline, output, scene->GetTlas()).Dispatch(1);
    auto miss = update.Readback(output->GetView());
    update.Compute(pipeline, output, old_tlas).Dispatch(1);
    auto old_hit = update.Readback(output->GetView());
    Submit(update);
    Require(Result(miss).ReadValue<uint32>() == 0 && Result(old_hit).ReadValue<uint32>() == 1, "TLAS history or instance transform update mismatch");
    for (auto& vertex : data)
        vertex.x += 20;
    CommandList refit;
    refit.CopyFrom(Bytes(data), vertices->GetView());
    refit.BuildAccelerationStructures({AccelerationStructureBuildParam{geometry, ERaytracingBuildMode::UPDATE}});
    refit.Compute(pipeline, output, old_tlas).Dispatch(1);
    auto refit_miss = refit.Readback(output->GetView());
    Submit(refit);
    Require(Result(refit_miss).ReadValue<uint32>() == 0, "BLAS update did not consume the changed vertices");
    auto                   aabb_buffer = device.CreateBuffer<float3>("procedural bounds", 2, EBufferUsageFlags::NONE);
    std::array<float3, 2>  bounds{float3{-1, -1, 1}, float3{1, 1, 2}};
    RaytracingGeometryInfo aabb_info{};
    aabb_info.segments.push_back(RaytracingSegment{.vertex_stride = 24, .primitive_count = 1, .vertex_buffer = aabb_buffer, .type = RTGT_AABBS});
    auto  aabb_geometry     = device.CreateRaytracingGeometry(aabb_info);
    auto  aabb_scene        = device.CreateRaytracingScene();
    auto& aabb_instance     = aabb_scene->AddInstance();
    aabb_instance.geom      = aabb_geometry.Get();
    aabb_instance.transform = Matrix3x4f(float4{1, 0, 0, 0}, float4{0, 1, 0, 0}, float4{0, 0, 1, 0});
    CommandList procedural;
    procedural.CopyFrom(Bytes(bounds), aabb_buffer->GetView());
    procedural.BuildAccelerationStructures({AccelerationStructureBuildParam{aabb_geometry, ERaytracingBuildMode::BUILD}});
    procedural.UpdateRaytracingScene(aabb_scene);
    procedural.Compute(pipeline, output, aabb_scene->GetTlas()).Dispatch(1);
    auto procedural_hit = procedural.Readback(output->GetView());
    Submit(procedural);
    Require(Result(procedural_hit).ReadValue<uint32>() == 2, "Inline procedural RayQuery missed the AABB BLAS");
    std::cout << "DXR BLAS, TLAS and inline RayQuery passed\n";
}

void IndirectAndRecovery() {
    auto&        device   = RenderDevice::Get();
    auto         compute  = ShaderManager::Get().Compute<BindlessCompute>("test/D3D12Backend.hlsl", "BindlessMain");
    auto         input    = device.CreateBuffer<uint32>("indirect input", 4, EBufferUsageFlags::NONE);
    auto         output   = device.CreateBuffer<uint32>("indirect output", 4, EBufferUsageFlags::UNORDERED_ACCESS);
    auto         indirect = device.CreateBuffer<uint32>("dispatch parameters", 3, EBufferUsageFlags::INDIRECT_BUFFER);
    auto         bindless = device.CreateBindlessArray(64);
    const uint32 handle   = bindless->AllocateBuffer(input->GetView());
    {
        CommandList dropped;
        dropped.UpdateBindlessArray(bindless);
    }
    std::array<uint32, 4> values{11, 12, 13, 14};
    std::array<uint32, 3> groups{4, 1, 1};
    CommandList           commands;
    commands.CopyFrom(Bytes(values), input->GetView());
    commands.CopyFrom(Bytes(groups), indirect->GetView());
    commands.UpdateBindlessArray(bindless);
    commands.Compute(compute, output, bindless, uint4{handle, 1, 0, 0}).DispatchIndirect(indirect->GetView());
    auto read = commands.Readback(output->GetView());
    Submit(commands);
    Require(Result(read).CopyAs<uint32>() == Array<uint32>({12, 13, 14, 15}), "Indirect dispatch or discarded bindless update recovery failed");
    class Unsupported final : public CustomCmd {
    public:
        Unsupported() :
            CustomCmd(CustomCmdId::CUSTOM_CMD_NONE) {}
        EQueueType GetQueueType() const override {
            return EQueueType::Graphics;
        }
    };
    auto                fence = device.CreateFence();
    std::atomic<uint32> retired{0}, succeeded{0};
    CommandList         rejected;
    rejected.ClearResource(output->GetView(), 99u);
    auto completion = rejected.TrackGpuCompletion("rejected recording");
    auto query      = rejected.BeginTimestampQuery("rejected query");
    rejected.EndTimestampQuery(query);
    rejected.Signal(fence, 1);
    rejected.AddCallback([&] {
        ++retired;
    });
    rejected.AddSuccessCallback([&] {
        ++succeeded;
    });
    rejected.AddCustomCommand(MakeUnique<Unsupported>());
    Submit(rejected);
    Require(completion.Status() == GpuCompletionStatus::Error && query.GetFuture().Status() == QueryStatus::Error && fence->IsRejected(1), "Rejected work reported successful GPU completion");
    Require(retired == 1 && succeeded == 0, "Rejected submit callbacks were not retired correctly");
    CommandList recovered;
    auto        recovered_read = recovered.Readback(output->GetView());
    Submit(recovered);
    Require(Result(recovered_read).CopyAs<uint32>() == Array<uint32>({12, 13, 14, 15}), "Failed recording modified resource contents or states");
    std::cout << "indirect dispatch, bindless discard, rejection and state rollback passed\n";
}

class WindowSource final : public WindowSurfaceSource {
public:
    explicit WindowSource(GLFWwindow* window) :
        window(window) {}
    WindowSurfaceIdentity GetIdentity() const noexcept override {
        return {EWindowSystemType::GLFW, uintptr_t(window), uintptr_t(glfwGetWin32Window(window)), 1};
    }
    WindowNativeHandle GetNativeWindow() const noexcept override {
        return {EWindowSystemType::GLFW, uintptr_t(window), uintptr_t(glfwGetWin32Window(window))};
    }

private:
    GLFWwindow* window;
};
void Presentation() {
    Require(glfwInit() != 0, "GLFW initialization failed");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    auto* window = glfwCreateWindow(64, 48, "D3D12 presentation contract", nullptr, nullptr);
    Require(window != nullptr, "D3D12 test window creation failed");
    {
        auto                source = std::make_shared<WindowSource>(window);
        SwapchainCreateInfo info{.surface = {source}, .size = {64, 48}, .preferred_format = PF_R8G8B8A8_UNORM};
        auto                swapchain = RenderDevice::Get().CreateSwapchain(info);
        for (uint32 i = 0; i < 3; ++i) {
            auto        texture = RenderDevice::Get().CreateTexture(info.size, info.preferred_format, ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::TRANSFER_SRC);
            CommandList commands;
            commands.ClearResource(texture->GetView(), float4{.25f, .5f, .75f, 1});
            Submit(commands);
            auto receipt = std::make_shared<PresentReceipt>();
            RHIExecutor::Get().Present(RHIPresentRequest{swapchain, texture->GetView(), receipt});
            RHIExecutor::Get().Sync(ERHISyncDepth::Present);
            Require(receipt->TryGetResult().has_value() && receipt->TryGetResult()->submitted, "D3D12 Present rejected a valid source");
            info.size = {80 + i * 8, 60 + i * 8};
            glfwSetWindowSize(window, int(info.size.x), int(info.size.y));
            glfwPollEvents();
            Require(swapchain->Recreate(info), "D3D12 swapchain resize failed");
        }
    }
    glfwDestroyWindow(window);
    glfwTerminate();
    std::cout << "presentation, receipts and repeated swapchain resize passed\n";
}
} // namespace

int main() {
    std::cout << std::unitbuf;
    bool tasks = false;
    try {
        ConfigManager::GetInstance().Init(MOER_TEST_WORKSPACE);
        ShaderCompiler::Init();
        TaskSystem::Init();
        tasks = true;
        RenderDevice::Init(DeviceInitInfo{.rhi_type = ERHIType::D3D12, .name = "D3D12BackendTest"});
        Transfers();
        Compute();
        SubresourcesAndClears();
        Raster();
        Multiview();
        Raytracing();
        IndirectAndRecovery();
        Presentation();
        RenderDevice::Get().FlushDebugMessages();
        Require(GetD3D12ValidationErrorCount() == 0, "D3D12 validation reported errors");
        RenderDevice::Dispose();
        TaskSystem::ShutDown();
        tasks = false;
        std::cout << "D3D12 backend contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "D3D12 backend contract failed: " << error.what() << '\n';
        if (RenderDevice::IsInitialized())
            RenderDevice::Dispose();
        if (tasks)
            TaskSystem::ShutDown();
        return 1;
    }
}
