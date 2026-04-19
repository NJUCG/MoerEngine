// bind bindless
#include <core/common/Bindless.hlsl>
#include <core/common/Common.hlsl>

BINDLESS_BINDINGS(3);

#include <materials/Material.hlsli>

#include <shared/Geometry.h>
#include <shared/ShaderParameters.h>
#include <shared/utils/MoerMath.hlsli>
#include <shared/utils/Packing.h>

#include <core/math/STL.hlsli>
#include <pipelines/raytracing/passes/GBufferUtils.hlsli>

#include <pipelines/raytracing/inline/RaytracingCommon.hlsli>

