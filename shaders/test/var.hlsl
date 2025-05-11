struct S0
{
    uint a;
    uint b;
    float2 _pad;
    float4 x;
};
struct S1
{
    uint a;
};

ConstantBuffer<S0> cb0 : register(b0, space1);
ConstantBuffer<S1> cb1 : register(b1, space0);
StructuredBuffer<float4> sb0 : register(t0, space2);
RWStructuredBuffer<S1> sb1 : register(u1, space2);
ByteAddressBuffer rb0 : register(t0, space3);
RWByteAddressBuffer rb1 : register(u0, space0);
Buffer<float> tb0 : register(t1, space99);
RWBuffer<float> tb1 : register(u0, space99);
Buffer<float> buf_arr[7] : register(t10, space0);

// Texture1D<float4> t0 : register(t0); // not support now
// Texture1DArray<float4> t1 : register(t1); // not support now
Texture2D<float4> t2 : register(t2);
Texture2DArray<float4> t3 : register(t3);
Texture3D<float4> t4 : register(t4);
TextureCube<float4> t5 : register(t5);
TextureCubeArray<float4> t6 : register(t6);
// Texture2DMS<float4, 4> t7 : register(t7);  // not support now
// Texture2DMSArray<float4, 8> t8 : register(t8);  // not support now
Texture2D<float4> tex_arr[9] : register(t19);

// RWTexture1D<float4> rwt0 : register(u0, space7); // not support now
// RWTexture1DArray<float4> rwt1 : register(u1, space7); // not support now
RWTexture2D<float4> rwt2 : register(u2, space7);
RWTexture2DArray<float4> rwt3 : register(u3, space7);
RWTexture3D<float4> rwt4 : register(u4, space7);

SamplerState s0 : register(s0);
// SamplerComparisonState s1 : register(s1); // not support now

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    float a = cb0.b;
    uint b = cb1.a;
    float c = 1;//sb0[0].x; // sb0 optimized out. so in cpp 'res' start from 1.
    float d = sb1[1].a;
    uint e = rb0.Load(2 * sizeof(float));
    float f = rb1.Load<float>(3 * sizeof(float));
    float g = tb0[4];
    float h = tb1[5];
    float i = buf_arr[6][6];


    float4 v0 = 1.f;// t0.Sample(s0, 0.5);
    float4 v1 = 1.f;// t1.Sample(s0, float2(0.5, 0.5));
    // float4 v2 = t2.Sample(s0, float2(0, 0));
    float4 v2 = t2.SampleLevel(s0, float2(0, 0.25), 1);
    float4 v3 = t3.Sample(s0, float3(0, 0, 0));
    float4 v4 = t4.Sample(s0, float3(0, 0, 0));
    float4 v5 = t5.Sample(s0, normalize(float3(1, 0.99, 0.99)));  // first face, topleft?? still zero..
    float4 v6 = t6.Sample(s0, float4(normalize(float3(1, 0.99, 0.99)), 0));
    // float4 v7 = t7.Load(tid.xy, 0);
    // float4 v8 = t8.Load(tid.xyz, 0);
    float4 v10 = tex_arr[6].Sample(s0, float2(0, 0));

    float4 w0 = 1.f;// rwt0[tid.x];
    float4 w1 = 1.f;// rwt1[tid.xy];
    float4 w2 = rwt2[tid.xy];
    float4 w3 = rwt3[tid.xyz];
    float4 w4 = rwt4[tid.xyz];

    float res = a * b * c * d * e * f * g * h * i;
    // res *= (v0 * v1 * v2 * v3 * v4 * v5 * v6 * v9 * v10 * w0 * w1 * w2 * w3 * w4).x;
    res *= (v0 * v1 * v2 * v3 * v4 * v5 * v6).x;
    rb1.Store(0, uint(res));
    
}