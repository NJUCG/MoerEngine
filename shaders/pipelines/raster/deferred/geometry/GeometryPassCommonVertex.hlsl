#ifndef SHADOW_DEPTH_PASS
#define SHADOW_DEPTH_PASS 0
#endif

#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
#include "shared/ShaderParameters.h"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/Geometry.h"
#include "shared/raster/ShaderParameters.h"
#include "shared/utils/Packing.h"

#include "pipelines/raster/deferred/geometry/VertexFactory.hlsl"

[[vk::push_constant]] ConstantBuffer<Moer::GeometryPassBindlessParam> param;

VertexFactory::VsOutput main(VertexFactory::VsInput input, uint instance_id : SV_InstanceID) {

    ArrayBuffer            instance_data_array = ArrayBuffer(param.instance_data);
    ArrayBuffer            geom_instance_array = ArrayBuffer(param.geometry_instance_data);
    Moer::GeometryInstance geom_instance = geom_instance_array.Load<Moer::GeometryInstance>(instance_id);
    Moer::InstanceData     instance_data = Moer::LoadInstanceData(
        instance_data_array.GetByteAddressBuffer(), geom_instance.instance_idx * sizeof(Moer::InstanceData)
    );

    return VertexFactory::GetConvertedAttributes(
        input, instance_data.model2world, param.world2clip, geom_instance.geom_idx
    );
}