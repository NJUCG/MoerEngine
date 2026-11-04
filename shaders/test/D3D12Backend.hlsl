#include "core/common/Bindless.hlsl"
BINDLESS_BINDINGS(1, 2, 3, 4)
DEFINE_SHADER_CONSTANT(uint4, params)

RWStructuredBuffer<uint> output;
RaytracingAccelerationStructure scene;
Buffer<float2> typed_input;
RWBuffer<uint> typed_output;

struct StructuredElement {
    float3 value;
    uint key;
    uint weight;
};
StructuredBuffer<StructuredElement> structured_input;
RWStructuredBuffer<StructuredElement> structured_output;

[numthreads(1, 1, 1)]
void StructuredMain(uint3 id : SV_DispatchThreadID) {
    StructuredElement item = structured_input[id.x];
    item.key += 100;
    structured_output[id.x] = item;
}

StructuredBuffer<StructuredElement> structured_inputs[] : register(t0, space7);
RWTexture2D<uint> array_outputs[] : register(u0, space7);

[numthreads(1, 1, 1)]
void ArrayMain(uint3 id : SV_DispatchThreadID) {
    array_outputs[id.x][uint2(0, 0)] = structured_inputs[id.x][0].key;
}

[numthreads(1, 1, 1)]
void TypedMain() {
    typed_output[0] = uint(typed_input[0].x + typed_input[0].y);
}

[numthreads(1, 1, 1)]
void BindlessMain(uint3 id : SV_DispatchThreadID) {
    output[id.x] = ArrayBuffer(moer_constants.params.x).Load<uint>(id.x) + moer_constants.params.y;
}

[numthreads(1, 1, 1)]
void RayMain() {
    RayDesc ray;
    ray.Origin = float3(0, 0, 0);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0.01;
    ray.TMax = 10;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 255, ray);
    while (query.Proceed()) {
        if (query.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE)
            query.CommitProceduralPrimitiveHit(1.0);
    }
    output[0] = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1 :
                query.CommittedStatus() == COMMITTED_PROCEDURAL_PRIMITIVE_HIT ? 2 : 0;
}

float4 VertexMain(uint vertex : SV_VertexID) : SV_Position {
    float2 positions[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    return float4(positions[vertex], 0.5, 1);
}
float4 PixelMain() : SV_Target {
    return float4(0.25, 0.5, 0.75, 1);
}

struct PrimitiveVertex {
    float4 position : SV_Position;
};

[maxvertexcount(3)]
void GeometryMain(triangle PrimitiveVertex input[3], inout TriangleStream<PrimitiveVertex> stream) {
    for (uint i = 0; i < 3; ++i)
        stream.Append(input[i]);
}

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void MeshMain(out vertices PrimitiveVertex verts[3], out indices uint3 triangles[1]) {
    SetMeshOutputCounts(3, 1);
    verts[0].position = float4(-1, -1, 0.5, 1);
    verts[1].position = float4(3, -1, 0.5, 1);
    verts[2].position = float4(-1, 3, 0.5, 1);
    triangles[0] = uint3(0, 1, 2);
}

struct MeshPayload {
    uint count;
};
groupshared MeshPayload mesh_payload;

[numthreads(1, 1, 1)]
void TaskMain() {
    mesh_payload.count = 3;
    DispatchMesh(1, 1, 1, mesh_payload);
}

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void TaskMeshMain(in payload MeshPayload data, out vertices PrimitiveVertex verts[3], out indices uint3 triangles[1]) {
    SetMeshOutputCounts(data.count, 1);
    verts[0].position = float4(-1, -1, 0.5, 1);
    verts[1].position = float4(3, -1, 0.5, 1);
    verts[2].position = float4(-1, 3, 0.5, 1);
    triangles[0] = uint3(0, 1, 2);
}

float4 OffsetVertexMain(uint vertex : SV_VertexID, uint instance : SV_InstanceID,
                       int base_vertex : SV_StartVertexLocation, uint base_instance : SV_StartInstanceLocation) : SV_Position {
    if (vertex % 3 == 0) {
        output[0] = uint(int(vertex) + base_vertex);
        output[1] = instance + base_instance;
    }
    float2 positions[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    return float4(positions[vertex % 3], 0.5, 1);
}
