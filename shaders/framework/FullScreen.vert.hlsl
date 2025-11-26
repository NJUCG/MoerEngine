struct VSOutput {
  float4 Pos : SV_POSITION;
  float2 UV : TEXCOORD0;
};

static const float2 Vertices[3] = {
    float2(-1.0, 3.0),
    float2(-1.0, -1.0),
    float2(3.0, -1.0),
};

VSOutput main(uint VertexIndex : SV_VertexID) {
  VSOutput output = (VSOutput)0;
  // pos revert y
  output.Pos = float4(Vertices[VertexIndex], 0.0, 1.0);
  output.UV =
      0.5 * (float2(Vertices[VertexIndex].x, -Vertices[VertexIndex].y) + 1.0);

  return output;
}