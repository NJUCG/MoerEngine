#include "rhi/RHICommand.h"

#include <iostream>
#include <stdexcept>

using namespace Moer;
using namespace Moer::Render;

namespace {
void Expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TrackedBuffer final : public Buffer {
public:
    explicit TrackedBuffer(int& destroyed) :
        Buffer(BufferInfo{64, 4, EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST}),
        destroyed(destroyed) {}
    ~TrackedBuffer() override {
        ++destroyed;
    }
    void SetName(std::string_view) override {}

private:
    int& destroyed;
};

class TrackedTexture final : public Texture {
public:
    explicit TrackedTexture(int& destroyed) :
        Texture(Info()),
        destroyed(destroyed) {}
    ~TrackedTexture() override {
        ++destroyed;
    }
    void SetName(std::string_view) override {}
    uint GetMipByteSize(uint) const override {
        return 16;
    }

private:
    static TextureInfo Info() {
        TextureInfo info{};
        info.extent = {2, 2};
        info.format = PF_R8G8B8A8_UNORM;
        info.usage  = ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED;
        return info;
    }
    int& destroyed;
};

class TrackedPipeline final : public PipelineState {
public:
    explicit TrackedPipeline(int& destroyed) :
        PipelineState(3),
        destroyed(destroyed) {}
    ~TrackedPipeline() override {
        ++destroyed;
    }

private:
    int& destroyed;
};

void RecordingAndCallbacksOwnDirectResources() {
    int         buffers = 0, textures = 0, pipelines = 0;
    CommandList list;
    {
        BufferRef       buffer(MoerNew(TrackedBuffer)(buffers));
        TextureRef      texture(MoerNew(TrackedTexture)(textures));
        ComputePipeline pipeline(PipelineHandle{MoerNew(TrackedPipeline)(pipelines)});
        list.CopyFrom(BufferView(buffer), BufferView(buffer));
        list.ClearResource(TextureView(texture), float4{});
        ArrayArguments args;
        args.args.emplace_back(BufferViewArray{BufferView(buffer), BufferView(buffer)});
        args.args.emplace_back(TextureViewArray{TextureView(texture)});
        auto cache = list.RegisterArgs(std::move(args));
        CommandList::ComputeDispatcher(pipeline, list, cache).DispatchIndirect(BufferView(buffer));
        Expect(buffer->GetRefCount() == 2, "one recording retained the same buffer more than once");
    }
    Expect(buffers + textures + pipelines == 0, "recording borrowed an external resource owner");
    bool ordinary = false, success = false;
    list.AddCallback([&] {
        Expect(buffers + textures + pipelines == 0, "resource died before ordinary callback");
        ordinary = true;
    });
    list.AddSuccessCallback([&] {
        Expect(ordinary && buffers + textures + pipelines == 0, "resource died before success callback");
        success = true;
    });
    {
        CmdSubmit submit = list.Submit();
        Expect(list.IsEmpty(), "sealing left an owner in the next recording generation");
        // Backends discard translated commands before releasing callback tiers.
        submit.cmds.clear();
        submit.cached_args.clear();
        for (auto& callback : submit.callbacks) {
            callback();
        }
        submit.callbacks.clear();
        for (auto& callback : submit.success_callbacks) {
            callback();
        }
        submit.success_callbacks.clear();
        Expect(success, "success callback was lost");
    }
    Expect(buffers == 1 && textures == 1 && pipelines == 1, "retirement leaked a direct resource");
}

void AttachmentsAndMeshInputsAreOwned() {
    int         buffers = 0, textures = 0, pipelines = 0;
    CommandList list;
    {
        BufferRef      vertices(MoerNew(TrackedBuffer)(buffers));
        BufferRef      indices(MoerNew(TrackedBuffer)(buffers));
        BufferRef      indirect(MoerNew(TrackedBuffer)(buffers));
        BufferRef      count(MoerNew(TrackedBuffer)(buffers));
        TextureRef     color(MoerNew(TrackedTexture)(textures));
        TextureRef     depth(MoerNew(TrackedTexture)(textures));
        PipelineHandle pipeline(MoerNew(TrackedPipeline)(pipelines));
        MeshDrawData   mesh;
        mesh.vtx_views.push_back(VertexBuffer{vertices.Get()});
        mesh.idx_view = IndexBuffer{BufferView(indices)};
        mesh.DrawIndirect(BufferView(indirect), BufferView(count), 1, 16);
        DrawBatch batch;
        batch.Emplace(ArrayArguments{}, pipeline).RegisterDrawData(std::move(mesh));
        list.Gfx("owned attachments", Rect2D{}, DepthAttachment{depth.Get()}, ColorAttachment{color.Get()})
            .AcceptDrawBatch(std::move(batch))
            .Dispatch();
    }
    Expect(buffers + textures + pipelines == 0, "draw recorded borrowed inputs");
    {
        auto submit = list.Submit();
        submit.cmds.clear();
        Expect(buffers + textures + pipelines == 0, "draw owners did not survive translation");
    }
    Expect(buffers == 4 && textures == 2 && pipelines == 1, "draw retirement leaked inputs");
}

void MergeMoveAndRejectionTransferOwners() {
    int         destroyed = 0;
    CommandList destination;
    {
        CommandList source;
        BufferRef   buffer(MoerNew(TrackedBuffer)(destroyed));
        source.ClearResource(BufferView(buffer), 0u);
        destination.AppendRecorded(std::move(source));
    }
    CommandList moved(std::move(destination));
    Expect(destroyed == 0 && destination.IsEmpty(), "move or merge dropped an owner");
    bool cleanup_ran = false;
    moved.AddCallback([&] {
        Expect(destroyed == 0, "rejection dropped resources before cleanup");
        cleanup_ran = true;
    });
    auto cleanup = moved.DrainOrdinaryCallbacksForRejection();
    Expect(moved.IsEmpty() && destroyed == 0, "rejection drain lost its resource owner");
    for (auto& callback : cleanup) {
        callback();
    }
    cleanup.clear();
    Expect(cleanup_ran && destroyed == 1, "rejection cleanup leaked resources");
}

void SubmitReplacementKeepsRejectedObserversAlive() {
    int         old_destroyed = 0, new_destroyed = 0;
    CommandList old_list, new_list;
    bool        rejected_observer_ran = false;
    old_list.TrackGpuCompletion("replaced generation").Then([&](const GpuCompletionResult&) {
        Expect(old_destroyed == 0, "replacement released resources before notifying rejection");
        rejected_observer_ran = true;
        // Re-entry must see the new, independent recording generation.
        Expect(old_list.IsEmpty(), "replacement observer reached stale recording payload");
    });
    {
        BufferRef old_buffer(MoerNew(TrackedBuffer)(old_destroyed));
        BufferRef new_buffer(MoerNew(TrackedBuffer)(new_destroyed));
        old_list.ClearResource(BufferView(old_buffer), 0u);
        new_list.ClearResource(BufferView(new_buffer), 0u);
    }
    {
        auto old_submit = old_list.Submit();
        auto new_submit = new_list.Submit();
        old_submit      = std::move(new_submit);
        Expect(rejected_observer_ran, "replacement did not notify its completion observer");
        Expect(old_destroyed == 1 && new_destroyed == 0, "submit replacement retained the wrong generation");
        Expect(!new_submit.lifetime, "moved-from submit still owns the payload");
    }
    Expect(new_destroyed == 1, "replacement submit leaked its resource");
}

void OpaqueOwnersSurviveSealing() {
    CommandList        list;
    auto               owner = std::make_shared<int>(7);
    std::weak_ptr<int> weak  = owner;
    list.KeepAlive(owner);
    owner.reset();
    Expect(!list.IsEmpty(), "owner-only recording was treated as empty");
    {
        auto submit = list.Submit();
        Expect(!weak.expired(), "opaque owner died at sealing");
    }
    Expect(weak.expired(), "opaque owner leaked after retirement");
}
} // namespace

int main() {
    try {
        RecordingAndCallbacksOwnDirectResources();
        AttachmentsAndMeshInputsAreOwned();
        MergeMoveAndRejectionTransferOwners();
        SubmitReplacementKeepsRejectedObserversAlive();
        OpaqueOwnersSurviveSealing();
        std::cout << "RHI resource lifetime tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
