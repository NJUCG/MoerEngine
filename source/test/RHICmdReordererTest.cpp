#include "rhi/vulkan/RHICmdReorderer.h"

#include <array>
#include <iostream>
#include <stdexcept>

using namespace Moer;
using namespace Moer::Render;

namespace {

void Expect(bool _condition, const char* _message) {
    if (!_condition) {
        throw std::runtime_error(_message);
    }
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

} // namespace

int main() {
    try {
        AccessHazardsAreLayeredWithoutReadAfterReadEdges();
        ReadBarrierParticipatesInFollowingWarDependency();
        MultiResourceBarrierPublishesEveryAccessAtItsFinalLayer();
        WriteBarrierWaitsForPriorRead();
        std::cout << "RHI command reorderer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RHI command reorderer test failed: " << error.what() << '\n';
        return 1;
    }
}
