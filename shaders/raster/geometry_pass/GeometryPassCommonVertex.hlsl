#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
#include "shared/ShaderParameters.h"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/Geometry.h"
#include "shared/utils/Packing.h"

#include "raster/geometry_pass/VertexFactory.hlsl"

struct Constsant {
    float4 color;
    uint texture;
    uint buffer;
    uint instance_data;
    uint geometry_data;
    uint geometry_instance_data;
    float4x4 camera_view_proj;
};
[[vk::push_constant]] ConstantBuffer<Constsant> param;
[[vk::binding(0, 0)]] ConstantBuffer<Moer::ViewParam> camera_buffer : register(b0);

VertexFactory::OutVertexAttributes main(VertexFactory::InVertexAttributes input, uint instance_id : SV_InstanceID) {

    ArrayBuffer instance_data_array = ArrayBuffer(param.instance_data);
    ArrayBuffer geom_instance_array = ArrayBuffer(param.geometry_instance_data);
    Moer::GeometryInstance geom_instance = geom_instance_array.Load<Moer::GeometryInstance>(instance_id);
    Moer::InstanceData instance_data = Moer::LoadInstanceData(instance_data_array.GetByteAddressBuffer(), geom_instance.instance_idx * sizeof(Moer::InstanceData));

    return VertexFactory::GetConvertedAttributes(input, instance_data.model2world, camera_buffer.world2clip, instance_id);
}