#include "test/VertexShaderFactory.hlsl"

void main(in VertexAttributes attribs, out VS_INPUT output) {
  output.pos = float4(attribs.position, 1.0f);
  output.normal = attribs.normal;
  output.uv = attribs.uv;
  output.instance_id = attribs.GetInstanceID(attribs);
  output.iid = attribs.GetInstanceID(attribs);
}