#include "rhi/vulkan/RHICmdReorderer.h"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

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
    TestBuffer(uint64 _elements, uint32 _stride, EBufferUsageFlags _usage) :
        Buffer(BufferInfo{_elements, _stride, _usage}) {}

    void SetName(const std::string_view) override {}
};

void ExpectRange(
    const UnorderedMap<Buffer*, BufferRange>& _ranges,
    Buffer*                                   _buffer,
    uint64                                    _min,
    uint64                                    _max,
    const char*                               _message
) {
    const auto it = _ranges.find(_buffer);
    Expect(it != _ranges.end() && it->second.min == _min && it->second.max == _max, _message);
}

bool FalseResourceFlag(uint64) {
    return false;
}

bool FalseBindlessMembership(uint64, uint64) {
    return false;
}

void NoopBindlessLock(uint64) {}

FunctionTable MakeFunctionTable() {
    return {
        .is_resource_write       = &FalseResourceFlag,
        .is_resource_read        = &FalseResourceFlag,
        .is_texture_sampled      = &FalseResourceFlag,
        .is_resource_in_bindless = &FalseBindlessMembership,
        .lock_bdls_array         = &NoopBindlessLock,
        .unlock_bdls_array       = &NoopBindlessLock,
    };
}

template<typename FirstAccess, typename SecondAccess>
void ExpectLayers(
    CmdReorderer::ResourceType _type,
    uint64                     _handle,
    FirstAccess&&              _first,
    SecondAccess&&             _second,
    int64                      _first_layer,
    int64                      _second_layer,
    const char*                _message
) {
    TCachedArgArray cached_args;
    CmdReorderer   reorderer(MakeFunctionTable(), cached_args);
    auto*          resource = reorderer.GetHandle(_handle, _type);
    const CmdReorderer::Range range;
    const int64 first_layer  = _first(reorderer, resource, range);
    const int64 second_layer = _second(reorderer, resource, range);
    Expect(first_layer == _first_layer && second_layer == _second_layer, _message);
}

void AccessHazardsAreLayeredWithoutReadAfterReadEdges() {
    constexpr std::array types{
        CmdReorderer::ResourceType::Texture_Buffer,
        CmdReorderer::ResourceType::Accel,
        CmdReorderer::ResourceType::Bindless,
    };
    auto read = [](CmdReorderer& _reorderer, CmdReorderer::ResourceHandle* _resource,
                   const CmdReorderer::Range& _range) {
        return _reorderer.SetRead(_resource, _range);
    };
    auto write = [](CmdReorderer& _reorderer, CmdReorderer::ResourceHandle* _resource,
                    const CmdReorderer::Range& _range) {
        return _reorderer.SetWrite(_resource, _range);
    };

    uint64 handle = 0x1000;
    for (const CmdReorderer::ResourceType type : types) {
        ExpectLayers(type, handle++, read, read, 0, 0, "read-after-read created a false dependency");
        ExpectLayers(type, handle++, write, read, 0, 1, "RAW dependency was not layered");
        ExpectLayers(type, handle++, read, write, 0, 1, "WAR dependency was not layered");
        ExpectLayers(type, handle++, write, write, 0, 1, "WAW dependency was not layered");
    }
}

int64 FindCommandLayer(const CmdReorderer& _reorderer, const Command* _command) {
    for (size_t layer = 0; layer < _reorderer.m_cmd_lists.size(); ++layer) {
        for (const auto* node = _reorderer.m_cmd_lists[layer].head; node != nullptr; node = node->next) {
            if (node->cmd == _command) {
                return static_cast<int64>(layer);
            }
        }
    }
    return -1;
}

void ReadBarrierParticipatesInFollowingWarDependency() {
    TCachedArgArray cached_args;
    CmdReorderer   reorderer(MakeFunctionTable(), cached_args);

    constexpr uint64 buffer_handle = 0x2000;
    std::array<byte, 16> data{};
    UploadBufferCmd first_write(buffer_handle, 0, data.size(), data.data());
    BarrierCmd      read_barrier(0, 0, 1, 0, EQueueType::Graphics, EQueueType::Graphics);
    read_barrier.ReadBuffer(
        BufferView(reinterpret_cast<Buffer*>(buffer_handle), 0, 4, 4),
        EBufferState::SHADER_RESOURCE,
        EPassType::Graphics
    );
    UploadBufferCmd following_write(buffer_handle, 0, data.size(), data.data());

    reorderer.AcceptCmd(&first_write);
    reorderer.AcceptCmd(&read_barrier);
    reorderer.AcceptCmd(&following_write);

    Expect(FindCommandLayer(reorderer, &first_write) == 0, "first write was not in layer 0");
    Expect(FindCommandLayer(reorderer, &read_barrier) == 1, "read barrier did not wait for prior write");
    Expect(
        FindCommandLayer(reorderer, &following_write) == 2,
        "write following a read barrier missed the WAR dependency"
    );
}

void MultiResourceBarrierPublishesEveryAccessAtItsFinalLayer() {
    TCachedArgArray cached_args;
    CmdReorderer   reorderer(MakeFunctionTable(), cached_args);

    constexpr uint64 buffer_a = 0x3000;
    constexpr uint64 buffer_b = 0x4000;
    std::array<byte, 16> data{};

    UploadBufferCmd write_a(buffer_a, 0, data.size(), data.data());
    UploadBufferCmd write_b_0(buffer_b, 0, data.size(), data.data());
    BarrierCmd read_b(0, 0, 1, 0, EQueueType::Graphics, EQueueType::Graphics);
    read_b.ReadBuffer(
        BufferView(reinterpret_cast<Buffer*>(buffer_b), 0, 4, 4),
        EBufferState::SHADER_RESOURCE,
        EPassType::Graphics
    );
    UploadBufferCmd write_b_1(buffer_b, 0, data.size(), data.data());
    BarrierCmd combined_read(0, 0, 2, 0, EQueueType::Graphics, EQueueType::Graphics);
    combined_read
        .ReadBuffer(
            BufferView(reinterpret_cast<Buffer*>(buffer_a), 0, 4, 4),
            EBufferState::SHADER_RESOURCE,
            EPassType::Graphics
        )
        .ReadBuffer(
            BufferView(reinterpret_cast<Buffer*>(buffer_b), 0, 4, 4),
            EBufferState::SHADER_RESOURCE,
            EPassType::Graphics
        );
    UploadBufferCmd write_a_after_barrier(buffer_a, 0, data.size(), data.data());

    reorderer.AcceptCmd(&write_a);
    reorderer.AcceptCmd(&write_b_0);
    reorderer.AcceptCmd(&read_b);
    reorderer.AcceptCmd(&write_b_1);
    reorderer.AcceptCmd(&combined_read);
    reorderer.AcceptCmd(&write_a_after_barrier);

    Expect(FindCommandLayer(reorderer, &combined_read) == 3, "multi-resource barrier used a provisional layer");
    Expect(
        FindCommandLayer(reorderer, &write_a_after_barrier) == 4,
        "multi-resource barrier did not publish buffer A read at its final layer"
    );
}

void WriteBarrierWaitsForPriorRead() {
    TCachedArgArray cached_args;
    CmdReorderer   reorderer(MakeFunctionTable(), cached_args);

    constexpr uint64 buffer_handle = 0x5000;
    BarrierCmd read_barrier(0, 0, 1, 0, EQueueType::Graphics, EQueueType::Graphics);
    read_barrier.ReadBuffer(
        BufferView(reinterpret_cast<Buffer*>(buffer_handle), 0, 4, 4),
        EBufferState::SHADER_RESOURCE,
        EPassType::Graphics
    );
    BarrierCmd write_barrier(0, 0, 0, 1, EQueueType::Graphics, EQueueType::Graphics);
    write_barrier.WriteBuffer(
        BufferView(reinterpret_cast<Buffer*>(buffer_handle), 0, 4, 4),
        EBufferState::UNORDERED_ACCESS,
        EPassType::Compute
    );

    reorderer.AcceptCmd(&read_barrier);
    reorderer.AcceptCmd(&write_barrier);

    Expect(FindCommandLayer(reorderer, &read_barrier) == 0, "initial read barrier was not in layer 0");
    Expect(FindCommandLayer(reorderer, &write_barrier) == 1, "write barrier missed the prior-read WAR edge");
}

void CommandResourcePreprocessingIsTaskGraphIndependent() {
    std::exception_ptr worker_error;
    std::jthread worker([&] {
        try {
            TestBuffer vertex_buffer(16, 4, EBufferUsageFlags::VERTEX_BUFFER);
            TestBuffer index_buffer(16, 4, EBufferUsageFlags::INDEX_BUFFER);
            TestBuffer indirect_buffer(16, 4, EBufferUsageFlags::INDIRECT_BUFFER);
            TestBuffer count_buffer(4, 4, EBufferUsageFlags::INDIRECT_BUFFER);

            MeshDrawData mesh{};
            mesh.vtx_views.emplace_back(VertexBuffer{&vertex_buffer, 12});
            mesh.idx_view = IndexBuffer{
                BufferView(&index_buffer, 8, 4, 4), EIndexElementType::IET_UINT32
            };
            mesh.DrawIndirect(
                BufferView(&indirect_buffer, 16, 8, 4),
                BufferView(&count_buffer, 4, 1, 4),
                8,
                4
            );

            PipelineHandle      pipeline{};
            ArrayArguments      args{};
            RenderPassInfo      pass_info{};
            Array<MeshDrawData> meshes{};
            meshes.emplace_back(std::move(mesh));
            SetDrawStateCmd draw_state(
                pipeline, std::move(args), std::move(pass_info), std::move(meshes)
            );
            ExpectRange(
                draw_state.VertexBuffers(), &vertex_buffer, 12, 76, "draw state missed vertex range"
            );
            ExpectRange(
                draw_state.IndexBuffers(), &index_buffer, 8, 24, "draw state missed index range"
            );
            ExpectRange(
                draw_state.IndirectBuffers(),
                &indirect_buffer,
                16,
                48,
                "draw state missed indirect range"
            );
            ExpectRange(
                draw_state.DrawCountBuffers(), &count_buffer, 4, 8, "draw state missed count range"
            );

            TestBuffer batch_vertex(32, 4, EBufferUsageFlags::VERTEX_BUFFER);
            TestBuffer batch_index(32, 4, EBufferUsageFlags::INDEX_BUFFER);
            TestBuffer batch_indirect(32, 4, EBufferUsageFlags::INDIRECT_BUFFER);
            TestBuffer dispatch_indirect(32, 4, EBufferUsageFlags::INDIRECT_BUFFER);
            TestBuffer dispatch_count(4, 4, EBufferUsageFlags::INDIRECT_BUFFER);

            MeshDrawData batch_mesh{};
            batch_mesh.vtx_views.emplace_back(VertexBuffer{&batch_vertex, 20});
            batch_mesh.idx_view = IndexBuffer{
                BufferView(&batch_index, 12, 6, 4), EIndexElementType::IET_UINT32
            };
            batch_mesh.DrawIndirect(BufferView(&batch_indirect, 24, 4, 4), 4, 4);

            DrawBatchElement mesh_element{};
            mesh_element.RegisterDrawData(std::move(batch_mesh));
            DrawBatchElement dispatch_element{};
            dispatch_element.RegisterMeshDispatch(DispatchMeshData::DispatchIndirectCount(
                BufferView(&dispatch_indirect, 32, 8, 4),
                BufferView(&dispatch_count, 8, 1, 4),
                8,
                4
            ));

            DrawBatch batch{};
            batch.draw_cmds.emplace_back(std::move(mesh_element));
            batch.draw_cmds.emplace_back(std::move(dispatch_element));
            MultiDrawCmd multi_draw(
                std::move(batch), RenderPassInfo{}, "TaskGraphIndependentMultiDraw"
            );
            ExpectRange(
                multi_draw.VertexBuffers(), &batch_vertex, 20, 148, "multi draw missed vertex range"
            );
            ExpectRange(
                multi_draw.IndexBuffers(), &batch_index, 12, 36, "multi draw missed index range"
            );
            ExpectRange(
                multi_draw.IndirectBuffers(),
                &batch_indirect,
                24,
                40,
                "multi draw missed mesh indirect range"
            );
            ExpectRange(
                multi_draw.IndirectBuffers(),
                &dispatch_indirect,
                32,
                64,
                "multi draw missed dispatch indirect range"
            );
            ExpectRange(
                multi_draw.IndirectBuffers(),
                &dispatch_count,
                8,
                12,
                "multi draw missed dispatch count range"
            );

            BufferRef accel_vertex(MoerNew(TestBuffer)(16, 4, EBufferUsageFlags::VERTEX_BUFFER));
            BufferRef accel_index(MoerNew(TestBuffer)(16, 4, EBufferUsageFlags::INDEX_BUFFER));
            RaytracingSegment segment{};
            segment.vertex_buffer = accel_vertex;
            segment.index_buffer  = accel_index;
            RaytracingGeometryInfo geometry_info{};
            geometry_info.segments.emplace_back(std::move(segment));
            RaytracingGeometryRef geometry(MoerNew(RaytracingGeometry)(geometry_info));
            Array<AccelerationStructureBuildParam> build_params{
                AccelerationStructureBuildParam{geometry, ERaytracingBuildMode::BUILD}
            };
            BuildAccelerationStructuresCmd build_accel(std::move(build_params));
            Expect(
                build_accel.VtxBuffers().find(accel_vertex.Get()) != build_accel.VtxBuffers().end(),
                "acceleration build missed vertex buffer"
            );
            Expect(
                build_accel.IdxBuffers().find(accel_index.Get()) != build_accel.IdxBuffers().end(),
                "acceleration build missed index buffer"
            );
        } catch (...) {
            worker_error = std::current_exception();
        }
    });
    worker.join();
    if (worker_error) {
        std::rethrow_exception(worker_error);
    }
}

} // namespace

int main() {
    try {
        AccessHazardsAreLayeredWithoutReadAfterReadEdges();
        ReadBarrierParticipatesInFollowingWarDependency();
        MultiResourceBarrierPublishesEveryAccessAtItsFinalLayer();
        WriteBarrierWaitsForPriorRead();
        CommandResourcePreprocessingIsTaskGraphIndependent();
        std::cout << "RHI command reorderer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RHI command reorderer test failed: " << error.what() << '\n';
        return 1;
    }
}
