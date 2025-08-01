#ifndef MORE_MESH_PROCESSOR_H
#define MORE_MESH_PROCESSOR_H
#include "ResourceAPI.h"
#include "math/Base.h"
#include "misc/CountableRef.h"
#include "misc/STL.h"
#include <stdint.h>

#include "rhi/RHICommon.h"

namespace Moer {
    struct MeshProcessInput {
        void*     vertex_data;
        uint32_t  vertex_count;
        uint32_t  vertex_stride;
        uint32_t* index_data;
        uint32_t  index_count;
    };

    class RESOURCE_API MeshProcessOutput {

    public:
        friend class MeshProcessor;
        Array<Render::MeshletDesc>  meshlets;
        Array<Render::MeshletBound> meshlet_bounds;
        Array<float>                meshlet_vertex_data;
        Array<uint32_t>             primitive_indices;
    };

    class RESOURCE_API MeshProcessor {
    public:
        MeshProcessor();
        static MeshProcessOutput GenerateMeshlets(const MeshProcessInput& input);

    private:
        static void GenerateMeshlets(const MeshProcessInput& input, MeshProcessOutput& output);
    };
};// namespace Moer
#endif