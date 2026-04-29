struct VertexOutput {
  float4 position : SV_Position;
};

VertexOutput main(uint vertex_id : SV_VertexID) {
  float2 position;
  if (vertex_id == 0) {
    position = float2(-1.0, -1.0);
  } else if (vertex_id == 1) {
    position = float2(3.0, -1.0);
  } else {
    position = float2(-1.0, 3.0);
  }

  VertexOutput output;
  output.position = float4(position, 0.0, 1.0);
  return output;
}
