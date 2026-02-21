#pragma once
#include <cstdint>

namespace Moer::Render::RenderGraph {

enum class ERDGBufferFlags : uint8;
enum class ERDGPassFlags : uint16;
enum class ERDGTextureFlags : uint8;
enum class ERDGUnorderedAccessViewFlags : uint8;

class FRDGBuffer;
using FRDGBufferRef = FRDGBuffer*;

struct FRDGBufferDesc;

class FRDGBuilder;

class FRDGPass;
using FRDGPassRef = FRDGPass*;

class FRDGPooledBuffer;

class FRDGPooledTexture;

class FRDGResource;
using FRDGResourceRef = FRDGResource*;

class FRDGTexture;
using FRDGTextureRef = FRDGTexture*;

class FRDGView;
class FRDGTextureSRV;
class FRDGTextureUAV;
class FRDGBufferSRV;
class FRDGBufferUAV;

} // namespace Moer::Render::RenderGraph
