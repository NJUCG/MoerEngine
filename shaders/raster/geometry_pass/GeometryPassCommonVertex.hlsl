#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
#include "shared/ShaderParameters.h"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/Geometry.h"
#include "shared/utils/Packing.h"
#include "shared/raster/geometry_pass/ShaderParameters.h"

#include "raster/geometry_pass/VertexFactory.hlsl"

[[vk::push_constant]] ConstantBuffer<Moer::GeometryPassBindlessParam> param;

[[vk::binding(0, 0)]] ConstantBuffer<Moer::ViewParam> view_param_buffer : register(b0);

VertexFactory::VsOutput main(VertexFactory::VsInput input, uint instance_id : SV_InstanceID) {

    ArrayBuffer instance_data_array = ArrayBuffer(param.instance_data);
    ArrayBuffer geom_instance_array = ArrayBuffer(param.geometry_instance_data);
    Moer::GeometryInstance geom_instance = geom_instance_array.Load<Moer::GeometryInstance>(instance_id);
    Moer::InstanceData instance_data = Moer::LoadInstanceData(instance_data_array.GetByteAddressBuffer(), geom_instance.instance_idx * sizeof(Moer::InstanceData));

    return VertexFactory::GetConvertedAttributes(input, instance_data.model2world, view_param_buffer.world2clip, geom_instance.geom_idx);
}