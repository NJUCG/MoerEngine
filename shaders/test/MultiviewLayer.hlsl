struct VsOutput {
    float4 position : SV_Position;
    nointerpolation uint view_id : TEXCOORD0;
};

VsOutput MultiviewVS(uint vertex_id : SV_VertexID, uint view_id : SV_ViewID) {
    float2 uv = float2((vertex_id << 1u) & 2u, vertex_id & 2u);
    VsOutput output;
    output.position = float4(uv * 2.0 - 1.0, 0.5, 1.0);
    output.view_id  = view_id;
    return output;
}

float4 MultiviewPS(VsOutput input) : SV_Target0 {
    uint view_id = min(input.view_id, 5u);
    float3 color = float3(
        view_id == 0u || view_id == 3u || view_id == 4u,
        view_id == 1u || view_id == 3u || view_id == 5u,
        view_id == 2u || view_id == 4u || view_id == 5u
    );
    return float4(color, 1.0);
}
