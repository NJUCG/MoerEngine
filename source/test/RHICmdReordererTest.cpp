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

class TestTexture final : public Texture {
public:
    explicit TestTexture(uint16_t _array_size = 1, uint8_t _num_mips = 1) :
        Texture(MakeInfo(_array_size, _num_mips)) {}

    uint GetMipByteSize(uint) const override {
        return 4;
    }

    void SetName(const std::string_view) override {}

private:
    static TextureInfo MakeInfo(uint16_t _array_size, uint8_t _num_mips) {
        TextureInfo info{};
        info.usage =
            ETextureUsageFlags::SAMPLED |
            ETextureUsageFlags::TRANSFER_DST;
        info.extent       = {4, 4};
        info.num_mips     = _num_mips;
        info.array_size   = _array_size;
        info.aspect_flags = ETextureAspectFlags::COLOR;
        return info;
    }
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

uint64 aliased_texture_resource = 0;
uint64 aliased_buffer_resource  = 0;
uint64 aliasing_bindless_handle = 0;

bool SelectedBindlessMembership(uint64 _resource, uint64 _bindless_handle) {
    return _bindless_handle == aliasing_bindless_handle &&
           (_resource == aliased_texture_resource ||
            _resource == aliased_buffer_resource);
}

class ScopedBindlessMembership final {
public:
    ScopedBindlessMembership(
        uint64 _texture_resource,
        uint64 _buffer_resource,
        uint64 _bindless_handle
    ) {
        aliased_texture_resource = _texture_resource;
        aliased_buffer_resource  = _buffer_resource;
        aliasing_bindless_handle = _bindless_handle;
    }

    ~ScopedBindlessMembership() {
        aliased_texture_resource = 0;
        aliased_buffer_resource  = 0;
        aliasing_bindless_handle = 0;
    }

    ScopedBindlessMembership(const ScopedBindlessMembership&) = delete;
    ScopedBindlessMembership& operator=(const ScopedBindlessMembership&) = delete;
};

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

FunctionTable MakeAliasingFunctionTable() {
    FunctionTable functions           = MakeFunctionTable();
    functions.is_resource_in_bindless = &SelectedBindlessMembership;
    return functions;
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

void ScopeCommandsOwnExclusiveLayers() {
    TCachedArgArray cached_args;
    CmdReorderer   reorderer(MakeFunctionTable(), cached_args);

    ScopeCmd renderer_push("Renderer", true, false, GpuMarkerPalette::Renderer());
    ScopeCmd pass_push("Pass", true, false, GpuMarkerPalette::Pass());
    std::array<byte, 16> first_data{};
    UploadBufferCmd first_work(0x6100, 0, first_data.size(), first_data.data());
    ScopeCmd pass_pop("Pass", false, false, GpuMarkerPalette::Pass());
    std::array<byte, 16> second_data{};
    UploadBufferCmd second_work(0x6200, 0, second_data.size(), second_data.data());
    ScopeCmd renderer_pop("Renderer", false, false, GpuMarkerPalette::Renderer());

    reorderer.AcceptCmd(&renderer_push);
    reorderer.AcceptCmd(&pass_push);
    reorderer.AcceptCmd(&first_work);
    reorderer.AcceptCmd(&pass_pop);
    reorderer.AcceptCmd(&second_work);
    reorderer.AcceptCmd(&renderer_pop);

    Expect(FindCommandLayer(reorderer, &renderer_push) == 0, "renderer push did not own layer 0");
    Expect(FindCommandLayer(reorderer, &pass_push) == 1, "nested pass push shared a marker layer");
    Expect(FindCommandLayer(reorderer, &first_work) == 2, "pass work escaped before its push marker");
    Expect(FindCommandLayer(reorderer, &pass_pop) == 3, "pass pop shared its work layer");
    Expect(FindCommandLayer(reorderer, &second_work) == 4, "following work remained inside prior pass");
    Expect(FindCommandLayer(reorderer, &renderer_pop) == 5, "renderer pop did not own the final layer");
}

void ResourceImportsMayFollowNonResourceOrderingBoundaries() {
    TCachedArgArray cached_args;
    CmdReorderer   reorderer(MakeFunctionTable(), cached_args);

    ScopeCmd root_scope(
        "Profiled Import",
        true,
        false,
        GpuMarkerPalette::Renderer(),
        EQueueType::Graphics
    );
    CommandList query_source(EQueueType::Graphics);
    QueryToken  timestamp =
        query_source.BeginTimestampQuery("Profiled Import");
    query_source.EndTimestampQuery(timestamp);
    CmdSubmit query_submit = query_source.Submit();
    Expect(
        query_submit.cmds.size() == 2 &&
            query_submit.cmds.front()->Type() == Command::EType::Query,
        "timestamp source did not emit its query pair"
    );

    TestTexture texture;
    TestBuffer  buffer(
        16,
        4,
        EBufferUsageFlags::UNORDERED_ACCESS |
            EBufferUsageFlags::TRANSFER_DST
    );
    QueueTransferCmd import(
        EQueueType::Copy,
        Array<ImportTexture>{
            ImportTexture(texture.GetView(), ETextureState::SAMPLE)
        },
        Array<ImportBuffer>{
            ImportBuffer(buffer.GetView(), EBufferState::UNORDERED_ACCESS)
        }
    );

    reorderer.AcceptCmd(&root_scope);
    reorderer.AcceptCmd(query_submit.cmds.front().get());
    reorderer.AcceptCmd(&import);

    Expect(
        FindCommandLayer(reorderer, &root_scope) == 0,
        "profile root did not retain its ordering boundary"
    );
    Expect(
        FindCommandLayer(reorderer, query_submit.cmds.front().get()) == 1,
        "timestamp begin did not retain its ordering boundary"
    );
    Expect(
        FindCommandLayer(reorderer, &import) == 2,
        "fresh resource import did not follow the profiling prefix"
    );

    auto* texture_handle = static_cast<CmdReorderer::RangeHandle*>(
        reorderer.GetHandle(
            uint64(&texture),
            CmdReorderer::ResourceType::Texture_Buffer
        )
    );
    auto* buffer_handle = static_cast<CmdReorderer::RangeHandle*>(
        reorderer.GetHandle(
            uint64(&buffer),
            CmdReorderer::ResourceType::Texture_Buffer
        )
    );
    Expect(
        reorderer.SetRead(
            texture_handle,
            CmdReorderer::Range(0, 1, 0, 1)
        ) == 3,
        "texture use did not wait for its profiled import"
    );
    Expect(
        reorderer.SetRead(
            buffer_handle,
            CmdReorderer::Range(0, buffer.GetByteSize())
        ) == 3,
        "buffer use did not wait for its profiled import"
    );

    QueryBackendAccess::ResolveErrorIfPending(
        timestamp, "reorderer import-prefix test cleanup"
    );
}

void ResourceImportsDetectPriorTextureAndBufferReads() {
    TCachedArgArray cached_args;
    CmdReorderer   reorderer(MakeFunctionTable(), cached_args);

    ScopeCmd root_scope(
        "Profiled Prior Read",
        true,
        false,
        GpuMarkerPalette::Renderer(),
        EQueueType::Graphics
    );
    CommandList query_source(EQueueType::Graphics);
    QueryToken  timestamp =
        query_source.BeginTimestampQuery("Profiled Prior Read");
    query_source.EndTimestampQuery(timestamp);
    CmdSubmit query_submit = query_source.Submit();
    Expect(
        query_submit.cmds.size() == 2 &&
            query_submit.cmds.front()->Type() == Command::EType::Query,
        "prior-read timestamp source did not emit its query pair"
    );

    TestTexture texture;
    TestBuffer  buffer(
        16,
        4,
        EBufferUsageFlags::UNORDERED_ACCESS |
            EBufferUsageFlags::TRANSFER_DST
    );
    auto* texture_handle = static_cast<CmdReorderer::RangeHandle*>(
        reorderer.GetHandle(
            uint64(&texture),
            CmdReorderer::ResourceType::Texture_Buffer
        )
    );
    auto* buffer_handle = static_cast<CmdReorderer::RangeHandle*>(
        reorderer.GetHandle(
            uint64(&buffer),
            CmdReorderer::ResourceType::Texture_Buffer
        )
    );
    const CmdReorderer::Range texture_range(0, 1, 0, 1);
    const CmdReorderer::Range buffer_range(0, buffer.GetByteSize());

    reorderer.AcceptCmd(&root_scope);
    reorderer.AcceptCmd(query_submit.cmds.front().get());
    Expect(
        reorderer.SetRead(texture_handle, texture_range) == 2,
        "texture prior read did not follow the profiling prefix"
    );
    Expect(
        reorderer.SetRead(buffer_handle, buffer_range) == 2,
        "buffer prior read did not follow the profiling prefix"
    );
    Expect(
        texture_handle->GetMaxReadLayer(texture_range) == 2 &&
            texture_handle->GetMaxWriteLayer(texture_range) < 0,
        "texture prior read was not distinguishable from a fresh import range"
    );
    Expect(
        buffer_handle->GetMaxReadLayer(buffer_range) == 2 &&
            buffer_handle->GetMaxWriteLayer(buffer_range) < 0,
        "buffer prior read was not distinguishable from a fresh import range"
    );
    const CmdReorderer::PriorResourceAccess texture_prior =
        reorderer.GetPriorResourceAccess(texture_handle, texture_range);
    const CmdReorderer::PriorResourceAccess buffer_prior =
        reorderer.GetPriorResourceAccess(buffer_handle, buffer_range);
    Expect(
        texture_prior.direct_exact_layer == 2 &&
            texture_prior.conservative_layer == 2,
        "texture prior read was missing from the exact import history"
    );
    Expect(
        buffer_prior.direct_exact_layer == 2 &&
            buffer_prior.conservative_layer == 2,
        "buffer prior read was missing from the exact import history"
    );

#if defined(NDEBUG)
    // Debug deliberately rejects this invalid ownership order with an
    // assertion. Release keeps a fail-safe dependency so the transfer can
    // never move before the already-recorded resource reads.
    QueueTransferCmd import(
        EQueueType::Copy,
        Array<ImportTexture>{
            ImportTexture(texture.GetView(), ETextureState::SAMPLE)
        },
        Array<ImportBuffer>{
            ImportBuffer(buffer.GetView(), EBufferState::UNORDERED_ACCESS)
        }
    );
    reorderer.AcceptCmd(&import);
    Expect(
        FindCommandLayer(reorderer, &import) == 3,
        "release import fallback did not wait for prior texture and buffer reads"
    );
    Expect(
        reorderer.SetRead(texture_handle, texture_range) == 4,
        "texture use did not wait for the release import fallback"
    );
    Expect(
        reorderer.SetRead(buffer_handle, buffer_range) == 4,
        "buffer use did not wait for the release import fallback"
    );
#endif

    QueryBackendAccess::ResolveErrorIfPending(
        timestamp, "reorderer prior-read test cleanup"
    );
}

void ResourceImportsDetectPriorTextureAndBufferWrites() {
    TCachedArgArray cached_args;
    CmdReorderer   reorderer(MakeFunctionTable(), cached_args);

    TestTexture texture;
    TestBuffer  buffer(
        16,
        4,
        EBufferUsageFlags::UNORDERED_ACCESS |
            EBufferUsageFlags::TRANSFER_DST
    );
    auto* texture_handle = static_cast<CmdReorderer::RangeHandle*>(
        reorderer.GetHandle(
            uint64(&texture),
            CmdReorderer::ResourceType::Texture_Buffer
        )
    );
    auto* buffer_handle = static_cast<CmdReorderer::RangeHandle*>(
        reorderer.GetHandle(
            uint64(&buffer),
            CmdReorderer::ResourceType::Texture_Buffer
        )
    );
    const CmdReorderer::Range texture_range(0, 1, 0, 1);
    const CmdReorderer::Range buffer_range(0, buffer.GetByteSize());

    Expect(
        reorderer.SetWrite(texture_handle, texture_range) == 0,
        "texture prior write did not enter the first resource layer"
    );
    Expect(
        reorderer.SetWrite(buffer_handle, buffer_range) == 0,
        "buffer prior write did not enter the first resource layer"
    );
    const CmdReorderer::PriorResourceAccess texture_prior =
        reorderer.GetPriorResourceAccess(texture_handle, texture_range);
    const CmdReorderer::PriorResourceAccess buffer_prior =
        reorderer.GetPriorResourceAccess(buffer_handle, buffer_range);
    Expect(
        texture_prior.direct_exact_layer == 0 &&
            texture_prior.conservative_layer == 0,
        "texture prior write was missing from the exact import history"
    );
    Expect(
        buffer_prior.direct_exact_layer == 0 &&
            buffer_prior.conservative_layer == 0,
        "buffer prior write was missing from the exact import history"
    );

#if defined(NDEBUG)
    QueueTransferCmd import(
        EQueueType::Copy,
        Array<ImportTexture>{
            ImportTexture(texture.GetView(), ETextureState::SAMPLE)
        },
        Array<ImportBuffer>{
            ImportBuffer(buffer.GetView(), EBufferState::UNORDERED_ACCESS)
        }
    );
    reorderer.AcceptCmd(&import);
    Expect(
        FindCommandLayer(reorderer, &import) == 1,
        "release import fallback did not wait for prior writes"
    );
    Expect(
        reorderer.SetRead(texture_handle, texture_range) == 2,
        "texture use did not wait for the prior-write import fallback"
    );
    Expect(
        reorderer.SetRead(buffer_handle, buffer_range) == 2,
        "buffer use did not wait for the prior-write import fallback"
    );
#endif
}

void VerifyOpaqueBindlessImportWait(
    bool   _current_membership,
    uint64 _bindless_handle
) {
    TCachedArgArray cached_args;
    TestTexture texture;
    TestBuffer  buffer(
        16,
        4,
        EBufferUsageFlags::UNORDERED_ACCESS |
            EBufferUsageFlags::TRANSFER_DST
    );
    ScopedBindlessMembership membership(
        _current_membership ? uint64(&texture) : 0,
        _current_membership ? uint64(&buffer) : 0,
        _current_membership ? _bindless_handle : 0
    );
    CmdReorderer reorderer(MakeAliasingFunctionTable(), cached_args);
    ScopeCmd root_scope(
        "Opaque Bindless Prior Access",
        true,
        false,
        GpuMarkerPalette::Renderer(),
        EQueueType::Graphics
    );
    reorderer.AcceptCmd(&root_scope);

    auto* texture_handle = static_cast<CmdReorderer::RangeHandle*>(
        reorderer.GetHandle(
            uint64(&texture),
            CmdReorderer::ResourceType::Texture_Buffer
        )
    );
    auto* buffer_handle = static_cast<CmdReorderer::RangeHandle*>(
        reorderer.GetHandle(
            uint64(&buffer),
            CmdReorderer::ResourceType::Texture_Buffer
        )
    );
    const CmdReorderer::Range texture_range(0, 1, 0, 1);
    const CmdReorderer::Range buffer_range(0, buffer.GetByteSize());

    UnorderedSet<uint64> no_current_writes;
    reorderer.VisitBindlessArg(_bindless_handle, no_current_writes);
    const int64 bindless_layer = reorderer.m_dispatch_layer;
    reorderer.RecordArgReads(bindless_layer);
    reorderer.m_max_bdls_layer =
        std::max(reorderer.m_max_bdls_layer, bindless_layer);
    Expect(
        bindless_layer == 1,
        "bindless access did not follow the profiling prefix"
    );
    Expect(
        texture_handle->GetMaxExactAccess(texture_range).layer < 0 &&
            buffer_handle->GetMaxExactAccess(buffer_range).layer < 0 &&
            texture_handle->GetExactAccessCount() == 0 &&
            buffer_handle->GetExactAccessCount() == 0,
        "bindless alias test unexpectedly created a direct resource access"
    );
    const CmdReorderer::PriorResourceAccess texture_prior =
        reorderer.GetPriorResourceAccess(texture_handle, texture_range);
    const CmdReorderer::PriorResourceAccess buffer_prior =
        reorderer.GetPriorResourceAccess(buffer_handle, buffer_range);
    Expect(
        texture_prior.direct_exact_layer < 0 &&
            texture_prior.conservative_layer == bindless_layer,
        "texture import did not conservatively wait for opaque bindless access"
    );
    Expect(
        buffer_prior.direct_exact_layer < 0 &&
            buffer_prior.conservative_layer == bindless_layer,
        "buffer import did not conservatively wait for opaque bindless access"
    );

    QueueTransferCmd import(
        EQueueType::Copy,
        Array<ImportTexture>{
            ImportTexture(texture.GetView(), ETextureState::SAMPLE)
        },
        Array<ImportBuffer>{
            ImportBuffer(buffer.GetView(), EBufferState::UNORDERED_ACCESS)
        }
    );
    reorderer.AcceptCmd(&import);
    Expect(
        FindCommandLayer(reorderer, &import) == bindless_layer + 1,
        "import moved before an opaque bindless access"
    );
    Expect(
        reorderer.SetRead(texture_handle, texture_range) ==
            bindless_layer + 2,
        "texture use did not wait for the bindless import fallback"
    );
    Expect(
        reorderer.SetRead(buffer_handle, buffer_range) ==
            bindless_layer + 2,
        "buffer use did not wait for the conservative bindless import"
    );

}

void ResourceImportsConservativelyWaitForOpaqueBindlessAccess() {
    // Current membership cannot describe historical membership. Both values
    // must therefore produce the same conservative ordering and neither may
    // turn into a Debug assertion without a direct resource access.
    VerifyOpaqueBindlessImportWait(true, 0xB1AD1E55);
    VerifyOpaqueBindlessImportWait(false, 0xB1AD1E56);
}

void QueueTransferWritesFeedBindlessAliasOrderingWithoutExactPollution() {
    TCachedArgArray cached_args;
    TestTexture texture;
    TestBuffer  buffer(
        16,
        4,
        EBufferUsageFlags::UNORDERED_ACCESS |
            EBufferUsageFlags::TRANSFER_DST
    );
    constexpr uint64 bindless_handle = 0xB1AD1E57;
    ScopedBindlessMembership membership(
        uint64(&texture),
        uint64(&buffer),
        bindless_handle
    );
    CmdReorderer reorderer(MakeAliasingFunctionTable(), cached_args);

    QueueTransferCmd import(
        EQueueType::Copy,
        Array<ImportTexture>{
            ImportTexture(texture.GetView(), ETextureState::SAMPLE)
        },
        Array<ImportBuffer>{
            ImportBuffer(buffer.GetView(), EBufferState::UNORDERED_ACCESS)
        }
    );
    reorderer.AcceptCmd(&import);
    Expect(
        reorderer.m_write_resources.contains(uint64(&texture)) &&
            reorderer.m_write_resources.contains(uint64(&buffer)),
        "QueueTransfer import did not publish texture and buffer writes for bindless tracking"
    );

    auto* texture_handle = static_cast<CmdReorderer::RangeHandle*>(
        reorderer.GetHandle(
            uint64(&texture),
            CmdReorderer::ResourceType::Texture_Buffer
        )
    );
    auto* buffer_handle = static_cast<CmdReorderer::RangeHandle*>(
        reorderer.GetHandle(
            uint64(&buffer),
            CmdReorderer::ResourceType::Texture_Buffer
        )
    );
    const CmdReorderer::Range texture_range(0, 1, 0, 1);
    const CmdReorderer::Range buffer_range(0, buffer.GetByteSize());
    Expect(
        texture_handle->GetExactAccessCount() == 1 &&
            buffer_handle->GetExactAccessCount() == 1,
        "QueueTransfer import did not create its exact write provenance"
    );

    reorderer.m_dispatch_layer = -1;
    reorderer.m_arg_read_resources.clear();
    reorderer.m_arg_write_resources.clear();
    reorderer.temp_writed_resources.clear();
    reorderer.VisitBindlessArg(
        bindless_handle,
        reorderer.temp_writed_resources
    );
    Expect(
        reorderer.m_dispatch_layer == 1,
        "bindless alias read did not wait for QueueTransfer import"
    );
    reorderer.RecordArgReads(reorderer.m_dispatch_layer);
    reorderer.m_max_bdls_layer = std::max(
        reorderer.m_max_bdls_layer,
        reorderer.m_dispatch_layer
    );

    Expect(
        texture_handle->GetMaxReadLayer(texture_range) == 1 &&
            buffer_handle->GetMaxReadLayer(buffer_range) == 1,
        "bindless membership read did not reach the conservative resource map"
    );
    Expect(
        texture_handle->GetMaxExactAccess(texture_range).layer == 0 &&
            buffer_handle->GetMaxExactAccess(buffer_range).layer == 0 &&
            texture_handle->GetExactAccessCount() == 1 &&
            buffer_handle->GetExactAccessCount() == 1,
        "frontend bindless membership polluted exact import provenance"
    );
}

void ExactImportHistoryIsBoundedAndMergesDuplicateRanges() {
    TCachedArgArray cached_args;
    CmdReorderer   reorderer(MakeFunctionTable(), cached_args);

    TestBuffer buffer(
        300,
        4,
        EBufferUsageFlags::UNORDERED_ACCESS |
            EBufferUsageFlags::TRANSFER_DST
    );
    TestTexture texture(132);
    auto* buffer_handle = static_cast<CmdReorderer::RangeHandle*>(
        reorderer.GetHandle(
            uint64(&buffer),
            CmdReorderer::ResourceType::Texture_Buffer
        )
    );
    auto* texture_handle = static_cast<CmdReorderer::RangeHandle*>(
        reorderer.GetHandle(
            uint64(&texture),
            CmdReorderer::ResourceType::Texture_Buffer
        )
    );
    const CmdReorderer::Range repeated_buffer_range(0, 4);
    const CmdReorderer::Range repeated_texture_range(0, 1, 0, 1);
    for (uint repeat = 0; repeat < 128; ++repeat) {
        reorderer.RecordRead(buffer_handle, repeated_buffer_range, 0);
        reorderer.RecordRead(texture_handle, repeated_texture_range, 0);
    }
    Expect(
        buffer_handle->GetExactAccessCount() == 1 &&
            texture_handle->GetExactAccessCount() == 1,
        "repeated exact ranges allocated duplicate history entries"
    );

    for (int64 index = 1; index < 64; ++index) {
        reorderer.RecordRead(
            buffer_handle,
            CmdReorderer::Range(index * 8, 4),
            0
        );
        reorderer.RecordRead(
            texture_handle,
            CmdReorderer::Range(0, 1, index * 2, 1),
            0
        );
    }
    Expect(
        buffer_handle->GetExactAccessCount() == 64 &&
            texture_handle->GetExactAccessCount() == 64 &&
            buffer_handle->GetMaxExactAccess(repeated_buffer_range).complete &&
            texture_handle->GetMaxExactAccess(repeated_texture_range).complete,
        "exact history became incomplete before reaching 64 unique ranges"
    );

    const CmdReorderer::Range overflow_buffer_range(64 * 8, 4);
    const CmdReorderer::Range overflow_texture_range(0, 1, 64 * 2, 1);
    reorderer.RecordRead(buffer_handle, overflow_buffer_range, 0);
    reorderer.RecordRead(texture_handle, overflow_texture_range, 0);
    reorderer.RecordRead(
        buffer_handle,
        CmdReorderer::Range(65 * 8, 4),
        0
    );
    reorderer.RecordRead(
        texture_handle,
        CmdReorderer::Range(0, 1, 65 * 2, 1),
        0
    );
    reorderer.RecordRead(buffer_handle, repeated_buffer_range, 7);
    reorderer.RecordRead(texture_handle, repeated_texture_range, 7);

    const CmdReorderer::RangeHandle::ExactAccessQuery buffer_overflow =
        buffer_handle->GetMaxExactAccess(overflow_buffer_range);
    const CmdReorderer::RangeHandle::ExactAccessQuery texture_overflow =
        texture_handle->GetMaxExactAccess(overflow_texture_range);
    Expect(
        buffer_handle->GetExactAccessCount() == 64 &&
            texture_handle->GetExactAccessCount() == 64 &&
            buffer_overflow.layer < 0 && !buffer_overflow.complete &&
            texture_overflow.layer < 0 && !texture_overflow.complete,
        "overflowing exact history allocated beyond its 64-range bound"
    );
    Expect(
        buffer_handle->GetMaxExactAccess(repeated_buffer_range).layer == 7 &&
            texture_handle->GetMaxExactAccess(repeated_texture_range).layer == 7,
        "incomplete exact history stopped merging a previously known range"
    );

    const CmdReorderer::PriorResourceAccess buffer_prior =
        reorderer.GetPriorResourceAccess(buffer_handle, overflow_buffer_range);
    const CmdReorderer::PriorResourceAccess texture_prior =
        reorderer.GetPriorResourceAccess(texture_handle, overflow_texture_range);
    Expect(
        buffer_prior.direct_exact_layer < 0 &&
            !buffer_prior.direct_exact_complete &&
            buffer_prior.conservative_layer == 0,
        "incomplete buffer history did not fall back to conservative ordering"
    );
    Expect(
        texture_prior.direct_exact_layer < 0 &&
            !texture_prior.direct_exact_complete &&
            texture_prior.conservative_layer == 0,
        "incomplete texture history did not fall back to conservative ordering"
    );

    QueueTransferCmd import(
        EQueueType::Copy,
        Array<ImportTexture>{
            ImportTexture(
                texture.GetView().Slice(128, 1),
                ETextureState::SAMPLE
            )
        },
        Array<ImportBuffer>{
            ImportBuffer(
                BufferView(&buffer, 64 * 8, 1, 4),
                EBufferState::UNORDERED_ACCESS
            )
        }
    );
    reorderer.AcceptCmd(&import);
    Expect(
        FindCommandLayer(reorderer, &import) == 1,
        "unknown overflow import ignored the bounded-history fallback"
    );
    Expect(
        buffer_handle->GetExactAccessCount() == 64 &&
            texture_handle->GetExactAccessCount() == 64,
        "overflow import allocated new exact-history entries"
    );
}

void ResourceImportExactHistorySurvivesReadWriteAndTwoDimensionalCompression() {
    TCachedArgArray cached_args;
    CmdReorderer   reorderer(MakeFunctionTable(), cached_args);

    TestBuffer read_buffer(
        64,
        4,
        EBufferUsageFlags::UNORDERED_ACCESS |
            EBufferUsageFlags::TRANSFER_DST
    );
    TestBuffer write_buffer(
        64,
        4,
        EBufferUsageFlags::UNORDERED_ACCESS |
            EBufferUsageFlags::TRANSFER_DST
    );
    TestTexture read_texture(34, 2);
    TestTexture write_texture(34, 2);

    auto* read_buffer_handle = static_cast<CmdReorderer::RangeHandle*>(
        reorderer.GetHandle(
            uint64(&read_buffer),
            CmdReorderer::ResourceType::Texture_Buffer
        )
    );
    auto* write_buffer_handle = static_cast<CmdReorderer::RangeHandle*>(
        reorderer.GetHandle(
            uint64(&write_buffer),
            CmdReorderer::ResourceType::Texture_Buffer
        )
    );
    auto* read_texture_handle = static_cast<CmdReorderer::RangeHandle*>(
        reorderer.GetHandle(
            uint64(&read_texture),
            CmdReorderer::ResourceType::Texture_Buffer
        )
    );
    auto* write_texture_handle = static_cast<CmdReorderer::RangeHandle*>(
        reorderer.GetHandle(
            uint64(&write_texture),
            CmdReorderer::ResourceType::Texture_Buffer
        )
    );

    constexpr int64 accessed_range_count = 17;
    for (int64 index = 0; index < accessed_range_count; ++index) {
        const CmdReorderer::Range buffer_range(index * 8, 4);
        const CmdReorderer::Range texture_range(
            index % 2,
            1,
            index * 2 + index % 2,
            1
        );
        Expect(
            reorderer.SetRead(read_buffer_handle, buffer_range) == 0 &&
                reorderer.SetRead(read_texture_handle, texture_range) == 0,
            "disjoint direct reads unexpectedly depended on each other"
        );
        Expect(
            reorderer.SetWrite(write_buffer_handle, buffer_range) == 0 &&
                reorderer.SetWrite(write_texture_handle, texture_range) == 0,
            "disjoint direct writes unexpectedly depended on each other"
        );
    }

    const CmdReorderer::Range touched_buffer_range(8, 4);
    const CmdReorderer::Range touched_texture_range(1, 1, 3, 1);
    const CmdReorderer::Range buffer_gap(4, 4);
    const CmdReorderer::Range texture_cross_gap(0, 1, 3, 1);
    Expect(
        read_buffer_handle->GetMaxReadLayer(buffer_gap) == 0 &&
            write_buffer_handle->GetMaxWriteLayer(buffer_gap) == 0 &&
            read_texture_handle->GetMaxReadLayer(texture_cross_gap) == 0 &&
            write_texture_handle->GetMaxWriteLayer(texture_cross_gap) == 0,
        "test did not trigger conservative read/write range compression"
    );

    const auto expect_touched = [&](CmdReorderer::RangeHandle* _handle,
                                    const CmdReorderer::Range& _range,
                                    const char* _message) {
        const auto exact = _handle->GetMaxExactAccess(_range);
        Expect(
            exact.layer == 0 && exact.complete &&
                _handle->GetExactAccessCount() == accessed_range_count,
            _message
        );
    };
    expect_touched(
        read_buffer_handle,
        touched_buffer_range,
        "compressed buffer read lost a touched exact range"
    );
    expect_touched(
        write_buffer_handle,
        touched_buffer_range,
        "compressed buffer write lost a touched exact range"
    );
    expect_touched(
        read_texture_handle,
        touched_texture_range,
        "compressed texture read lost a touched exact range"
    );
    expect_touched(
        write_texture_handle,
        touched_texture_range,
        "compressed texture write lost a touched exact range"
    );

    const auto expect_gap = [&](CmdReorderer::RangeHandle* _handle,
                                const CmdReorderer::Range& _range,
                                const char* _message) {
        const auto exact = _handle->GetMaxExactAccess(_range);
        const auto prior = reorderer.GetPriorResourceAccess(_handle, _range);
        Expect(
            exact.layer < 0 && exact.complete &&
                prior.direct_exact_layer < 0 &&
                prior.direct_exact_complete &&
                prior.conservative_layer < 0,
            _message
        );
    };
    expect_gap(
        read_buffer_handle,
        buffer_gap,
        "exact history falsely marked a compressed buffer-read gap"
    );
    expect_gap(
        write_buffer_handle,
        buffer_gap,
        "exact history falsely marked a compressed buffer-write gap"
    );
    expect_gap(
        read_texture_handle,
        texture_cross_gap,
        "exact history falsely marked a texture-read mip-array cross-gap"
    );
    expect_gap(
        write_texture_handle,
        texture_cross_gap,
        "exact history falsely marked a texture-write mip-array cross-gap"
    );

#if defined(NDEBUG)
    QueueTransferCmd touched_import(
        EQueueType::Copy,
        Array<ImportTexture>{
            ImportTexture(
                read_texture.GetView(1, 1).Slice(3, 1),
                ETextureState::SAMPLE
            ),
            ImportTexture(
                write_texture.GetView(1, 1).Slice(3, 1),
                ETextureState::SAMPLE
            )
        },
        Array<ImportBuffer>{
            ImportBuffer(
                read_buffer.GetView(8, 4),
                EBufferState::UNORDERED_ACCESS
            ),
            ImportBuffer(
                write_buffer.GetView(8, 4),
                EBufferState::UNORDERED_ACCESS
            )
        }
    );
    reorderer.AcceptCmd(&touched_import);
    Expect(
        FindCommandLayer(reorderer, &touched_import) == 1,
        "NDEBUG touched-range import missed its exact fallback"
    );
#endif

    QueueTransferCmd gap_import(
        EQueueType::Copy,
        Array<ImportTexture>{
            ImportTexture(
                read_texture.GetView(0, 1).Slice(3, 1),
                ETextureState::SAMPLE
            ),
            ImportTexture(
                write_texture.GetView(0, 1).Slice(3, 1),
                ETextureState::SAMPLE
            )
        },
        Array<ImportBuffer>{
            ImportBuffer(
                read_buffer.GetView(4, 4),
                EBufferState::UNORDERED_ACCESS
            ),
            ImportBuffer(
                write_buffer.GetView(4, 4),
                EBufferState::UNORDERED_ACCESS
            )
        }
    );
    reorderer.AcceptCmd(&gap_import);
    Expect(
        FindCommandLayer(reorderer, &gap_import) == 0,
        "untouched compressed gap import gained a false dependency"
    );
    Expect(
        reorderer.SetRead(read_buffer_handle, buffer_gap) == 1 &&
            reorderer.SetRead(write_buffer_handle, buffer_gap) == 1 &&
            reorderer.SetRead(read_texture_handle, texture_cross_gap) == 1 &&
            reorderer.SetRead(write_texture_handle, texture_cross_gap) == 1,
        "gap resource use did not wait for its successful import"
    );
}

void MultiviewAttachmentsPublishEveryLayerWrite() {
    TCachedArgArray cached_args;
    CmdReorderer   reorderer(MakeFunctionTable(), cached_args);

    TestTexture draw_color(8);
    RenderPassInfo draw_pass{};
    draw_pass.color_attachments.emplace_back(ColorAttachment{
        .target      = &draw_color,
        .action      = AC_CLEAR_STORE,
        .mip_level   = 0,
        .array_layer = 1,
        .array_count = 6,
    });

    PipelineHandle      pipeline{};
    ArrayArguments      args{};
    Array<MeshDrawData> meshes{};
    SetDrawStateCmd     draw(
        pipeline, std::move(args), std::move(draw_pass), std::move(meshes), "MultiviewSetDraw"
    );
    reorderer.AcceptCmd(&draw);

    auto* draw_color_handle = reorderer.GetHandle(
        uint64(&draw_color), CmdReorderer::ResourceType::Texture_Buffer
    );
    Expect(
        reorderer.SetRead(draw_color_handle, CmdReorderer::Range(0, 1, 6, 1)) == 1,
        "set-draw attachment did not publish the last multiview layer write"
    );
    Expect(
        reorderer.SetRead(draw_color_handle, CmdReorderer::Range(0, 1, 7, 1)) == 0,
        "set-draw attachment published a write outside its layer range"
    );

    TestTexture multi_draw_depth(8);
    RenderPassInfo multi_draw_pass{};
    multi_draw_pass.depth_attachment.target       = &multi_draw_depth;
    multi_draw_pass.depth_attachment.action       = AC_DS_CLEAR_STORE;
    multi_draw_pass.depth_attachment.mip_level    = 0;
    multi_draw_pass.depth_attachment.array_layer  = 1;
    multi_draw_pass.depth_attachment.array_count  = 6;

    DrawBatch    batch{};
    MultiDrawCmd multi_draw(
        std::move(batch), std::move(multi_draw_pass), "MultiviewMultiDraw"
    );
    reorderer.AcceptCmd(&multi_draw);

    auto* multi_draw_depth_handle = reorderer.GetHandle(
        uint64(&multi_draw_depth), CmdReorderer::ResourceType::Texture_Buffer
    );
    Expect(
        reorderer.SetRead(multi_draw_depth_handle, CmdReorderer::Range(0, 1, 6, 1)) == 1,
        "multi-draw attachment did not publish the last multiview layer write"
    );
    Expect(
        reorderer.SetRead(multi_draw_depth_handle, CmdReorderer::Range(0, 1, 7, 1)) == 0,
        "multi-draw attachment published a write outside its layer range"
    );
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
        ScopeCommandsOwnExclusiveLayers();
        ResourceImportsMayFollowNonResourceOrderingBoundaries();
        ResourceImportsDetectPriorTextureAndBufferReads();
        ResourceImportsDetectPriorTextureAndBufferWrites();
        ResourceImportsConservativelyWaitForOpaqueBindlessAccess();
        QueueTransferWritesFeedBindlessAliasOrderingWithoutExactPollution();
        ExactImportHistoryIsBoundedAndMergesDuplicateRanges();
        ResourceImportExactHistorySurvivesReadWriteAndTwoDimensionalCompression();
        MultiviewAttachmentsPublishEveryLayerWrite();
        CommandResourcePreprocessingIsTaskGraphIndependent();
        std::cout << "RHI command reorderer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RHI command reorderer test failed: " << error.what() << '\n';
        return 1;
    }
}
