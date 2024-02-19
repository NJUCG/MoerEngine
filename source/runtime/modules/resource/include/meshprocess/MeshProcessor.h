#ifndef MORE_MESH_PROCESSOR_H
#define MORE_MESH_PROCESSOR_H
#include "ResourceAPI.h"
#include "math/Base.h"
#include "misc/CountableRef.h"
#include "misc/STL.h"
#include <stdint.h>

namespace Moer {

    class MeshResource : public Countable {
    public:
        virtual void Destroy() override {
            delete this;
        };
    };
    class MeshProcessOutput;
    using MeshProcessOutputRef = CountableRef<MeshProcessOutput>;

    // using MeshResourceRef = CountableRef<MeshResource>;
    struct MeshletDesc {
        uint32_t vertex_offset;
        uint32_t vertex_count;
        uint32_t primitive_offset;
        uint32_t primitive_count;
    };

    struct MeshletBound {
        /* bounding sphere, useful for frustum and occlusion culling */
        Vector3f center;
        float    radius;

        /* normal cone axis and cutoff, stored in 8-bit SNORM format; decode using x/127.0 */
        int8_t cone_axis_s8[3];
        int8_t cone_cutoff; /* = cos(angle/2) */

        /* bool reject = dot(center - camera_position, cone_axis) >= cone_cutoff* length(center - camera_position) + radius; */
    };

    struct MeshProcessInput {
        void*     vertex_data;
        uint32_t  vertex_count;
        uint32_t  vertex_stride;
        uint32_t* index_data;
        uint32_t  index_count;
    };

    class RESOURCE_API MeshProcessOutput : public MeshResource {

    private:
        friend class MeshProcessor;
        Array<MeshletDesc>  meshlets;
        Array<MeshletBound> meshlet_bounds;
        Array<float>        meshlet_vertex_data;
        Array<uint8_t>      primitive_indices;
    };

    class RESOURCE_API MeshProcessor {
    public:
        static MeshProcessOutputRef GenerateMeshlets(const MeshProcessInput& input);

    private:
        static void GenerateMeshlets(const MeshProcessInput& input, MeshProcessOutput& output);
    };
};// namespace Moer
#endif