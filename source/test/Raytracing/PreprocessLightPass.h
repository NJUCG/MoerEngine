#ifndef MOER_PREPARE_LIGHTS_PASS_H
#define MOER_PREPARE_LIGHTS_PASS_H

#include "RTResource.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "rhi/RHIResource.h"

namespace Moer {
    class Scene;
}
namespace Moer::Render {
    class PrepareLightPass {
    public:
        PrepareLightPass(class RenderDevice& _device, class ShaderManager& _manager, Scene& _scene);
        void Process(class CommandList& _cmd_list, RTContext& _rt_ctx);
        void CountEmissiveInstances(uint& _num_emissive_meshes, uint& _num_emissive_triangles);

    private:
        class RenderDevice&  device;
        class ShaderManager& manager;
        Scene&               scene;

        UnorderedMap<uint64, uint> instance_light_buffer_offsets;
        UnorderedMap<uint64, uint> primitive_light_buffer_offsets;

        BufferRef geom_instance_to_light_buf;

        bool b_odd_frame = false;
    };

}// namespace Moer::Render

#endif