
struct Constants {
    uint frame_cnt;
    float time;
    uint2 _pad;
};
ConstantBuffer<Constants> args : register(b0);
RWTexture2D<float4> tex : register(u0);

// adopt from https://www.shadertoy.com/view/mtyGWy
float3 palette( float t ) {
    float3 a = float3(0.5, 0.5, 0.5);
    float3 b = float3(0.5, 0.5, 0.5);
    float3 c = float3(1.0, 1.0, 1.0);
    float3 d = float3(0.263,0.416,0.557);

    return a + b*cos( 6.28318*(c*t+d) );
}

[numthreads(8, 8, 1)]
void main(int3 tid : SV_DispatchThreadID)
{
    uint w, h;
    tex.GetDimensions(w, h);

    // tex[tid.xy] = float4(tid.xy / float2(w, h), 0, 1);

    float iTime = args.time / 500;
    float2 uv = (tid.xy * 2.0 - float2(w, h)) / h;
    float2 uv0 = uv;
    float3 finalColor = float3(0.0, 0.0, 0.0);
    
    for (float i = 0.0; i < 4.0; i++) {
        uv = frac(uv * 1.5) - 0.5;
        float d = length(uv) * exp(-length(uv0));
        float3 col = palette(length(uv0) + i*.4 + iTime*.4);
        d = sin(d*8. + iTime)/8.;
        d = abs(d);
        d = pow(0.01 / d, 1.2);
        finalColor += col * d;
    }
    tex[tid.xy] = float4(finalColor, 1.0);

}
