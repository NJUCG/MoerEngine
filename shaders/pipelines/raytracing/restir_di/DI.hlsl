// bind bindless
#include <core/common/Bindless.hlsl>
#include <core/common/Common.hlsl>

BINDLESS_BINDINGS(3, 2, 4, 5);

#include <materials/Material.hlsl>

#include <shared/Geometry.h>
#include <shared/ShaderParameters.h>
#include <shared/utils/MoerMath.hlsli>
#include <shared/utils/Packing.h>

#include <core/math/STL.hlsli>
#include <hwrt/GBufferUtils.hlsli>

#include <pipelines/raytracing/inline/RaytracingCommon.hlsli>

