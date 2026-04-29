struct Args {
  float4 color;
};

[[vk::push_constant]] ConstantBuffer<Args> args : register(b0);

void main(out float4 target : SV_Target) {
  target = args.color;
}
