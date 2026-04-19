#ifndef FRAMEWORK_BINDLESS_COMMON_HLSL
#define FRAMEWORK_BINDLESS_COMMON_HLSL

#define DEBUG_MODE 1
#define NUM_STATIC_SAMPLERS 256
#define BINDLESS_INDIRECTION_NAME g__array_bindless
#define BINDLESS_SAMPLER_INDEX_BITS 8u
#define BINDLESS_SAMPLER_INDEX_MASK ((1u << BINDLESS_SAMPLER_INDEX_BITS) - 1u)
#define BINDLESS_TEXTURE_INDEX_SHIFT BINDLESS_SAMPLER_INDEX_BITS
#define BINDLESS_TEXTURE_DESCRIPTOR_INDEX(packed_handle) ((packed_handle) >> BINDLESS_TEXTURE_INDEX_SHIFT)
#define BINDLESS_SAMPLER_DESCRIPTOR_INDEX(packed_handle) ((packed_handle) & BINDLESS_SAMPLER_INDEX_MASK)

uint ReadBindlessPackedHandle(uint idx);
ByteAddressBuffer AccessGlobalBufferHeap(uint idx);
SamplerState AccessGlobalSamplerHeap(uint idx);

template<typename TextureValue>
Texture2D<TextureValue> AccessGlobalTexture2DHeap(uint idx);

Texture2D AccessGlobalTexture2DUntypedHeap(uint idx);

template<typename TextureValue>
TextureCube<TextureValue> AccessGlobalTextureCubeHeap(uint idx);

TextureCube AccessGlobalTextureCubeUntypedHeap(uint idx);

struct RenderResourceHandle {
  // 23 bits for index, 2 bits to indicate resource type, 1 bit for writability,
  // 6 bits for version
  uint index;
  bool IsValid() { return index != ~0; }
  uint ResourceTag() { return (index >> 23) & ((1 << 2) - 1); }
  bool IsWritable() { return (index >> 25) && 1; }
  uint Version() { return (index >> 26) & ((1 << 6) - 1); }
  uint ReadIndex() {
#if DEBUG_MODE
    return index & ((1 << 23) - 1);
#else
    return index;
#endif
  }

#if VULKAN
  uint WriteIndex() { return ReadIndex(); }
#else
  uint WriteIndex() { return ReadIndex() + 1; }
#endif
  void LogInfo() {
    printf("[hlsl] index: %d, ResourceTag: %d, IsWritable: %d, Version: %d, "
           "ReadIndex: %d, WriteIndex: %d\n",
           index, ResourceTag(), IsWritable() ? 1 : 0, Version(), ReadIndex(),
           WriteIndex());
  }
};

struct ByteBufferHandle {
  uint internalIndex;
};

struct SamplerHeapHandle {
  uint internalIndex;
};

struct ArrayBuffer {
  uint handle;

  ByteAddressBuffer GetByteAddressBuffer() {
    uint array_handle = ReadBindlessPackedHandle(handle);
    return AccessGlobalBufferHeap(array_handle);
  }

  template<typename ReadStructure>
  ReadStructure Load(uint index) {
    return GetByteAddressBuffer().Load<ReadStructure>(sizeof(ReadStructure) * index);
  }

  template<typename ReadStructure>
  ReadStructure Load(uint index, uint offset) {
    return GetByteAddressBuffer().Load<ReadStructure>(sizeof(ReadStructure) * index + offset);
  }
};

struct TextureHandle {
  uint handle;

  uint PackedHandle() {
    return ReadBindlessPackedHandle(handle);
  }

  uint TextureIndex() {
    return BINDLESS_TEXTURE_DESCRIPTOR_INDEX(PackedHandle());
  }

  uint SamplerIndex() {
    return BINDLESS_SAMPLER_DESCRIPTOR_INDEX(PackedHandle());
  }

  template<typename TextureValue>
  Texture2D<TextureValue> GetTexture2D() {
    return AccessGlobalTexture2DHeap<TextureValue>(TextureIndex());
  }

  Texture2D GetTexture2D() {
    return AccessGlobalTexture2DUntypedHeap(TextureIndex());
  }

  template<typename TextureValue>
  TextureValue Sample2D(float2 uv) {
    return GetTexture2D<TextureValue>().Sample(AccessGlobalSamplerHeap(SamplerIndex()), uv, int2(0, 0));
  }

  template<typename TextureValue>
  TextureValue SampleLevel(float2 uv, float level = 0.f) {
    return GetTexture2D<TextureValue>().SampleLevel(
      AccessGlobalSamplerHeap(SamplerIndex()), uv, level, int2(0, 0));
  }

  float4 SampleLevel(float2 uv, float level) {
    return GetTexture2D().SampleLevel(AccessGlobalSamplerHeap(SamplerIndex()), uv, level, int2(0, 0));
  }

  template<typename TextureValue>
  TextureValue SampleGrad(float2 uv, float2 grad_x, float2 grad_y) {
    return GetTexture2D<TextureValue>().SampleGrad(
      AccessGlobalSamplerHeap(SamplerIndex()), uv, grad_x, grad_y, int2(0, 0));
  }

  template<typename TextureValue>
  TextureValue SampleCube(float3 uv) {
    TextureCube<TextureValue> tex = AccessGlobalTextureCubeHeap<TextureValue>(TextureIndex());
    return tex.Sample(AccessGlobalSamplerHeap(SamplerIndex()), uv);
  }

  float4 SampleCube(float3 uv) {
    TextureCube tex = AccessGlobalTextureCubeUntypedHeap(TextureIndex());
    return tex.Sample(AccessGlobalSamplerHeap(SamplerIndex()), uv);
  }

  template<typename TextureValue>
  TextureValue SampleLevelCube(float3 uv, float level = 0.f) {
    TextureCube<TextureValue> tex = AccessGlobalTextureCubeHeap<TextureValue>(TextureIndex());
    return tex.SampleLevel(AccessGlobalSamplerHeap(SamplerIndex()), uv, level);
  }

  template<typename TextureValue>
  TextureValue SampleGradCube(float3 uv, float3 grad_x, float3 grad_y) {
    TextureCube<TextureValue> tex = AccessGlobalTextureCubeHeap<TextureValue>(TextureIndex());
    return tex.SampleGrad(AccessGlobalSamplerHeap(SamplerIndex()), uv, grad_x, grad_y);
  }
};

struct SamplerHandle {
  uint handle;

  SamplerState GetSampler() {
    return AccessGlobalSamplerHeap(handle);
  }
};

#if VULKAN
#define BINDLESS_BINDINGS(BufferSpace) \
  [[vk::binding(0, BufferSpace)]] StructuredBuffer<uint> BINDLESS_INDIRECTION_NAME; \
  uint ReadBindlessPackedHandle(uint idx) { return BINDLESS_INDIRECTION_NAME[NonUniformResourceIndex(idx)]; } \
  ByteAddressBuffer AccessGlobalBufferHeap(uint idx) { return ByteAddressBuffer(ResourceDescriptorHeap[NonUniformResourceIndex(idx)]); } \
  SamplerState AccessGlobalSamplerHeap(uint idx) { return (SamplerState)SamplerDescriptorHeap[NonUniformResourceIndex(idx)]; } \
  template<typename TextureValue> \
  Texture2D<TextureValue> AccessGlobalTexture2DHeap(uint idx) { return Texture2D<TextureValue>(ResourceDescriptorHeap[NonUniformResourceIndex(idx)]); } \
  Texture2D AccessGlobalTexture2DUntypedHeap(uint idx) { return Texture2D(ResourceDescriptorHeap[NonUniformResourceIndex(idx)]); } \
  template<typename TextureValue> \
  TextureCube<TextureValue> AccessGlobalTextureCubeHeap(uint idx) { return TextureCube<TextureValue>(ResourceDescriptorHeap[NonUniformResourceIndex(idx)]); } \
  TextureCube AccessGlobalTextureCubeUntypedHeap(uint idx) { return TextureCube(ResourceDescriptorHeap[NonUniformResourceIndex(idx)]); }

#elif DXIL

#define BINDLESS_BINDINGS(BufferSpace) \
  [[vk::binding(0, BufferSpace)]] StructuredBuffer<uint> BINDLESS_INDIRECTION_NAME; \
  uint ReadBindlessPackedHandle(uint idx) { return BINDLESS_INDIRECTION_NAME[NonUniformResourceIndex(idx)]; } \
  ByteAddressBuffer AccessGlobalBufferHeap(uint idx) { return ByteAddressBuffer(ResourceDescriptorHeap[NonUniformResourceIndex(idx)]); } \
  SamplerState AccessGlobalSamplerHeap(uint idx) { return (SamplerState)SamplerDescriptorHeap[NonUniformResourceIndex(idx)]; } \
  template<typename TextureValue> \
  Texture2D<TextureValue> AccessGlobalTexture2DHeap(uint idx) { return Texture2D<TextureValue>(ResourceDescriptorHeap[NonUniformResourceIndex(idx)]); } \
  Texture2D AccessGlobalTexture2DUntypedHeap(uint idx) { return Texture2D(ResourceDescriptorHeap[NonUniformResourceIndex(idx)]); } \
  template<typename TextureValue> \
  TextureCube<TextureValue> AccessGlobalTextureCubeHeap(uint idx) { return TextureCube<TextureValue>(ResourceDescriptorHeap[NonUniformResourceIndex(idx)]); } \
  TextureCube AccessGlobalTextureCubeUntypedHeap(uint idx) { return TextureCube(ResourceDescriptorHeap[NonUniformResourceIndex(idx)]); }

#endif// VULKAN/DXIL

#endif// FRAMEWORK_BINDLESS_COMMON_HLSL
