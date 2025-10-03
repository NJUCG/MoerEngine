#ifndef MOER_SHADER_HEADERS_GEOMETRY_H
#define MOER_SHADER_HEADERS_GEOMETRY_H

#ifdef __cplusplus
#include "misc/Traits.h"
#include <algorithm>
#ifndef CONST
#define CONST constexpr
#endif
#define GLOBAL_FUNC static
namespace Moer::Render {
#else
#define CONST const
#define GLOBAL_FUNC
namespace Moer {
#endif
    //SOA arranged geometry data
    struct GeometryData {

        uint num_indices;
        uint num_vertices;
        uint index_buffer_handle;
        uint index_offset;

        uint vertex_buffer_handle;
        uint vertex_offset;
        uint prev_vertex_offset;//for skinning
        uint texcoord0_offset;

        uint texcoord1_offset;
        uint normal_offset;
        uint tangent_offset;
        uint mat_idx_and_type;//mat_idx:24, type:8

        uint GetMaterialIdx() { return mat_idx_and_type >> 8; }
    };

    struct InstanceData {
        uint     first_geom_idx;
        uint     geom_count;
        uint     first_geom_instance_idx;
        uint     padding;
        float3x4 model2world;
        float3x4 prev_model2world;
    };

    struct GeometryInstance {
        uint geom_idx;
        uint instance_idx;
    };

    static CONST uint g_size_of_triangle_indices = 12;
    static CONST uint g_size_of_position         = 12;
    static CONST uint g_size_of_texcoord         = 8;
    static CONST uint g_size_of_normal           = 4;
    static CONST uint g_size_of_joint_indices    = 8;
    static CONST uint g_size_of_joint_weights    = 16;

#ifndef __cplusplus

    InstanceData LoadInstanceData(ByteAddressBuffer _buf, uint _offset) {
        uint4 d0 = _buf.Load4(_offset);

        uint4 d1 = _buf.Load4(_offset + 16 * 1);
        uint4 d2 = _buf.Load4(_offset + 16 * 2);
        uint4 d3 = _buf.Load4(_offset + 16 * 3);

        uint4 d4 = _buf.Load4(_offset + 16 * 4);
        uint4 d5 = _buf.Load4(_offset + 16 * 5);
        uint4 d6 = _buf.Load4(_offset + 16 * 6);

        InstanceData res;
        res.first_geom_idx          = d0.x;
        res.geom_count              = d0.y;
        res.first_geom_instance_idx = d0.z;
        res.padding                 = d0.w;

        res.model2world = float3x4(
            asfloat(d1),
            asfloat(d2),
            asfloat(d3));

        res.prev_model2world = float3x4(
            asfloat(d4),
            asfloat(d5),
            asfloat(d6));

        return res;
    }

    GeometryData LoadGeometryData(ByteAddressBuffer _buf, uint _offset) {
        uint4 d0 = _buf.Load4(_offset);
        uint4 d1 = _buf.Load4(_offset + 16 * 1);
        uint4 d2 = _buf.Load4(_offset + 16 * 2);

        GeometryData res;
        res.num_indices         = d0.x;
        res.num_vertices        = d0.y;
        res.index_buffer_handle = d0.z;
        res.index_offset        = d0.w;

        res.vertex_buffer_handle = d1.x;
        res.vertex_offset        = d1.y;
        res.prev_vertex_offset   = d1.z;
        res.texcoord0_offset     = d1.w;

        res.texcoord1_offset = d2.x;
        res.normal_offset    = d2.y;
        res.tangent_offset   = d2.z;
        res.mat_idx_and_type = d2.w;

        return res;
    }

#else
#endif
}
#undef CONST
#undef GLOBAL_FUNC
#endif