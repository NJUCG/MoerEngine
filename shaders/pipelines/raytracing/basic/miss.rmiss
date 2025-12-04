// Copyright 2020 Google LLC

[[vk::binding(0,1)]] cbuffer clearValue{
    float3 color;
}

struct [raypayload] Payload
{
[[vk::location(0)]] float3 hitValue;
};
[shader("miss")]
void main(inout Payload p)
{
    p.hitValue = color;
}