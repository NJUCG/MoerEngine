// bind bindless
#include <framework/Bindless.hlsl>
#include <framework/Common.hlsl>

BINDLESS_BINDINGS(3, 2, 4, 5);

#include <framework/Material.hlsl>

#include <shared/Geometry.h>
#include <shared/ShaderParameters.h>
#include <shared/utils/MoerMath.hlsli>
#include <shared/utils/Packing.h>

#include <MathLib/STL.hlsli>
#include <hwrt/GBufferUtils.hlsli>

#include <framework/RaytracingCommon.hlsli>

