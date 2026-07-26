#pragma once

#include "RTResource.h"
#include "misc/STL.h"
#include "rendergraph/RenderGraph.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"

#include <atomic>

namespace Moer::Render::Raytracing {

class RaytracingFrameSetupPass {
public:
    struct AcceptedSnapshot {
        BindlessArrayRef   bindless_array{};
        RaytracingSceneRef scene{};
        RaytracingTlasRef  current_tlas{};
        RaytracingTlasRef  previous_tlas{};
        BufferRef          current_tlas_buffer{};
        BufferRef          previous_tlas_buffer{};
        uint64             tlas_revision = 0;
        bool               tlas_built    = false;

        [[nodiscard]] bool IsValid() const {
            return bindless_array && scene && current_tlas && previous_tlas &&
                   current_tlas_buffer && previous_tlas_buffer;
        }
    };

    struct RecordPayload {
        BindlessArrayRef   bindless_array{};
        RaytracingSceneRef scene{};
        RaytracingTlasRef  destination_tlas{};
        RaytracingTlasRef  previous_tlas{};
        BufferRef          destination_tlas_buffer{};
        BufferRef          previous_tlas_buffer{};
        BufferRef          instance_buffer{};
        BufferRef          scratch_buffer{};

        Array<RaytracingGeometryRef> geometries;
        Array<BufferRef>             geometry_buffers;
        Array<BufferRef>             scene_buffers;
        Array<TextureRef>            scene_textures;

        UniquePtr<Command> build_tlas_command{};
        std::atomic_bool   build_tlas_recorded{false};

        uint64 target_revision      = 0;
        bool   build_tlas           = false;
        bool   full_instance_upload = false;
        bool   scene_inputs_updated = false;
        bool   external_tlas_built  = false;
    };

    struct PreparedCommand {
        SharedPtr<RecordPayload> record{};

        [[nodiscard]] explicit operator bool() const {
            return record && record->bindless_array && record->scene &&
                   record->destination_tlas && record->destination_tlas_buffer;
        }

        [[nodiscard]] bool BuildsTlas() const {
            return record && record->build_tlas;
        }
    };

    PreparedCommand Prepare(
        const RTContext&    rt_ctx,
        RaytracingSceneRef  scene,
        BindlessArrayRef    bindless_array,
        uint64              target_revision,
        bool                build_tlas,
        bool                scene_inputs_updated,
        bool                external_tlas_built
    );

    bool AddPasses(RenderGraph& graph, const PreparedCommand& command) const;
    bool ProcessLinear(CommandList& cmd_list, const PreparedCommand& command) const;

    [[nodiscard]] AcceptedSnapshot CommitAccepted(const PreparedCommand& command) noexcept;
    void                           ResetAcceptedResources() noexcept;

private:
    UnorderedSet<const Buffer*> accepted_tlas_buffers;
    UnorderedSet<const Buffer*> accepted_instance_buffers;
    UnorderedSet<const Buffer*> accepted_scratch_buffers;
    UnorderedSet<const Buffer*> accepted_scene_buffers;
};

} // namespace Moer::Render::Raytracing
