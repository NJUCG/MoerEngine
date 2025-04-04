#ifndef FRAMEWORK_BINDLESS_COMMON_HLSL
#define FRAMEWORK_BINDLESS_COMMON_HLSL

#define DEBUG_MODE 1
#define NUM_STATIC_SAMPLERS 256
#define BINDLESS_SUFFIX _114514_bdls
#define BINDLESS_ARRAY_SUFFIX _array_114514_bdls
#define BINDLESS_NAME_SUFFIX _
#define CONCAT(x, y) x##y
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

#define ITERATE_TEXTURE_TYPES(ITERATOR, ...)                                   \
  ITERATOR(int, ##__VA_ARGS__)                                                 \
  ITERATOR(uint, ##__VA_ARGS__)                                                \
  ITERATOR(float, ##__VA_ARGS__)                                               \
  ITERATOR(int2, ##__VA_ARGS__)                                                \
  ITERATOR(uint2, ##__VA_ARGS__)                                               \
  ITERATOR(float2, ##__VA_ARGS__)                                              \
  ITERATOR(int3, ##__VA_ARGS__)                                                \
  ITERATOR(uint3, ##__VA_ARGS__)                                               \
  ITERATOR(float3, ##__VA_ARGS__)                                              \
  ITERATOR(int4, ##__VA_ARGS__)                                                \
  ITERATOR(uint4, ##__VA_ARGS__)                                               \
  ITERATOR(float4, ##__VA_ARGS__)

#define INNER_GENERATE_TEXTURE_TYPE_SLOT(NativeType, TextureType, Binding,     \
                                         Set)                                  \
  [[vk::binding(Binding, Set)]] TextureType<NativeType>                        \
      g##TextureType##NativeType##__114514_bdls[];

#define DEFINE_TEXTURE_TYPE_AND_FORMATS_SLOTS(TextureType, Binding, Set)       \
  ITERATE_TEXTURE_TYPES(INNER_GENERATE_TEXTURE_TYPE_SLOT, TextureType,         \
                        Binding, Set)                                          \
  [[vk::binding(Binding, Set)]] TextureType g##TextureType##__114514_bdls[];

struct ByteBufferHandle {
  uint internalIndex;
};

struct TlasHandle {
  uint internalIndex;
};
template <typename T> struct Texture1DHandle {
  uint internalIndex;
};

struct Texture1DHandleNative {
  uint internalIndex;
};

template <typename T> struct Texture2DHandle {
  uint internalIndex;
};

struct Texture2DHandleNative {
  uint internalIndex;
};

template <typename T> struct Texture3DHandle {
  uint internalIndex;
};

struct Texture3DHandleNative {
  uint internalIndex;
};

template <typename T> struct Texture1DSampleHandle {
  uint internalIndex;
};

struct Texture1DSampleHandleNative {
  uint internalIndex;
};

template <typename T> struct Texture2DSampleHandle {
  uint internalIndex;
};

struct Texture2DSampleHandleNative {
  uint internalIndex;
};

template <typename T> struct Texture3DSampleHandle {
  uint internalIndex;
};

struct Texture3DSampleHandleNative {
  uint internalIndex;
};

struct SamplerHeapHandle {
  uint internalIndex;
};

//////////////////////////////////////////////////////////////////////////////////////
// TextureType<NativeType> fetch, Texture2D<float4>
//////////////////////////////////////////////////////////////////////////////////////

#define INNER_GENERATE_TEXTURE_TYPE_FETCH(NativeType, TextureType)             \
  TextureType<NativeType> operator[](TextureType##Handle<NativeType> handle) { \
    uint tex_handle =                                                          \
        g__array_114514_bdls[NonUniformResourceIndex(handle.internalIndex)];   \
    uint tex_idx = tex_handle >> 8;                                            \
    return TextureType<NativeType>(                                            \
        g##TextureType##NativeType##__114514_bdls[NonUniformResourceIndex(     \
            tex_idx)]);                                                        \
  }

//////////////////////////////////////////////////////////////////////////////////////
// TextureType fetch, Texture2D, Texture3D, TextureCube
//////////////////////////////////////////////////////////////////////////////////////

#define INNER_GENERATE_TEXTURE_TYPE_FETCH_WITHOUT_TEMPLATE(TextureType)        \
  TextureType operator[](TextureType##HandleNative handle) {                   \
    uint tex_handle =                                                          \
        g__array_114514_bdls[NonUniformResourceIndex(handle.internalIndex)];   \
    uint tex_idx = tex_handle >> 8;                                            \
    return TextureType(                                                        \
        g##TextureType##__114514_bdls[NonUniformResourceIndex(tex_idx)]);      \
  }

//////////////////////////////////////////////////////////////////////////////////////
// TextureType<NativeType> Sample, Texture2D<float4>
//////////////////////////////////////////////////////////////////////////////////////

#define INNER_GENERATE_TEXTURE_TYPE_SAMPLE(NativeType, TextureType, CoordType, \
                                           OffsetType)                         \
  NativeType Sample(TextureType##SampleHandle<NativeType> handle,              \
                    CoordType uv, OffsetType offset) {                         \
    uint tex_handle =                                                          \
        g__array_114514_bdls[NonUniformResourceIndex(handle.internalIndex)];   \
    uint tex_idx = tex_handle >> 8;                                            \
    uint sampler_idx = tex_handle & 0xff;                                      \
    TextureType<NativeType> tex = TextureType<NativeType>(                     \
        g##TextureType##NativeType##__114514_bdls[NonUniformResourceIndex(     \
            tex_idx)]);                                                        \
    return tex.Sample(                                                         \
        gsampler__114514_bdls[NonUniformResourceIndex(sampler_idx)], uv,       \
        offset);                                                               \
  }

//////////////////////////////////////////////////////////////////////////////////////
// TextureType Sample, Texture2D, Texture3D, TextureCube
//////////////////////////////////////////////////////////////////////////////////////

#define INNER_GENERATE_TEXTURE_TYPE_SAMPLE_WITHOUT_TEMPLATE(                   \
    TextureType, CoordType, OffsetType)                                        \
  float4 Sample(TextureType##SampleHandleNative handle, CoordType uv,          \
                OffsetType offset) {                                           \
    uint tex_handle =                                                          \
        g__array_114514_bdls[NonUniformResourceIndex(handle.internalIndex)];   \
    uint tex_idx = tex_handle >> 8;                                            \
    uint sampler_idx = tex_handle & 0xff;                                      \
    TextureType tex = TextureType(                                             \
        g##TextureType##__114514_bdls[NonUniformResourceIndex(tex_idx)]);      \
    return tex.Sample(                                                         \
        gsampler__114514_bdls[NonUniformResourceIndex(sampler_idx)], uv,       \
        offset);                                                               \
  }

//////////////////////////////////////////////////////////////////////////////////////
// TextureType<NativeType> SampleLevel, Texture2D<float4>
//////////////////////////////////////////////////////////////////////////////////////

#define INNER_GENERATE_TEXTURE_TYPE_SAMPLE_LEVEL(NativeType, TextureType,      \
                                                 CoordType, OffsetType)        \
  NativeType SampleLevel(TextureType##SampleHandle<NativeType> handle,         \
                         CoordType uv, OffsetType offset, float level) {       \
    uint tex_handle =                                                          \
        g__array_114514_bdls[NonUniformResourceIndex(handle.internalIndex)];   \
    uint tex_idx = tex_handle >> 8;                                            \
    uint sampler_idx = tex_handle & 0xff;                                      \
    TextureType<NativeType> tex = TextureType<NativeType>(                     \
        g##TextureType##NativeType##__114514_bdls[NonUniformResourceIndex(     \
            tex_idx)]);                                                        \
    return tex.SampleLevel(                                                    \
        gsampler__114514_bdls[NonUniformResourceIndex(sampler_idx)], uv,       \
        level, offset);                                                        \
  }

//////////////////////////////////////////////////////////////////////////////////////
// TextureType SampleLevel, Texture2D, Texture3D, TextureCube
//////////////////////////////////////////////////////////////////////////////////////

#define INNER_GENERATE_TEXTURE_TYPE_SAMPLE_LEVEL_WITHOUT_TEMPLATE(             \
    TextureType, CoordType, OffsetType)                                        \
  float4 SampleLevel(TextureType##SampleHandleNative handle, CoordType uv,     \
                     OffsetType offset, float level) {                         \
    uint tex_handle =                                                          \
        g__array_114514_bdls[NonUniformResourceIndex(handle.internalIndex)];   \
    uint tex_idx = tex_handle >> 8;                                            \
    uint sampler_idx = tex_handle & 0xff;                                      \
    TextureType tex = TextureType(                                             \
        g##TextureType##__114514_bdls[NonUniformResourceIndex(tex_idx)]);      \
    return tex.SampleLevel(                                                    \
        gsampler__114514_bdls[NonUniformResourceIndex(sampler_idx)], uv,       \
        level, offset);                                                        \
  }

//////////////////////////////////////////////////////////////////////////////////////
// TextureType<NativeType> SampleGrad, Texture2D<float4>
//////////////////////////////////////////////////////////////////////////////////////

#define INNER_GENERATE_TEXTURE_TYPE_SAMPLE_GRAD(NativeType, TextureType,       \
                                                CoordType, OffsetType)         \
  NativeType SampleGrad(TextureType##SampleHandle<NativeType> handle,          \
                        CoordType uv, OffsetType offset, CoordType grad_x,     \
                        CoordType grad_y) {                                    \
    uint tex_handle =                                                          \
        g__array_114514_bdls[NonUniformResourceIndex(handle.internalIndex)];   \
    uint tex_idx = tex_handle >> 8;                                            \
    uint sampler_idx = tex_handle & 0xff;                                      \
    TextureType<NativeType> tex = TextureType<NativeType>(                     \
        g##TextureType##NativeType##__114514_bdls[NonUniformResourceIndex(     \
            tex_idx)]);                                                        \
    return tex.SampleGrad(                                                     \
        gsampler__114514_bdls[NonUniformResourceIndex(sampler_idx)], uv,       \
        grad_x, grad_y, offset);                                               \
  }

//////////////////////////////////////////////////////////////////////////////////////
// TextureType SampleGrad, Texture2D, Texture3D, TextureCube
//////////////////////////////////////////////////////////////////////////////////////

#define INNER_GENERATE_TEXTURE_TYPE_SAMPLE_GRAD_WITHOUT_TEMPLATE(              \
    TextureType, CoordType, OffsetType)                                        \
  float4 SampleGrad(TextureType##SampleHandleNative handle, CoordType uv,      \
                    OffsetType offset, CoordType grad_x, CoordType grad_y) {   \
    uint tex_handle =                                                          \
        g__array_114514_bdls[NonUniformResourceIndex(handle.internalIndex)];   \
    uint tex_idx = tex_handle >> 8;                                            \
    uint sampler_idx = tex_handle & 0xff;                                      \
    TextureType tex = TextureType(                                             \
        g##TextureType##__114514_bdls[NonUniformResourceIndex(tex_idx)]);      \
    return tex.SampleGrad(                                                     \
        gsampler__114514_bdls[NonUniformResourceIndex(sampler_idx)], uv,       \
        grad_x, grad_y, offset);                                               \
  }

#define DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(TextureType, CoordType,          \
                                              OffsetType)                      \
  ITERATE_TEXTURE_TYPES(INNER_GENERATE_TEXTURE_TYPE_FETCH, TextureType)        \
  ITERATE_TEXTURE_TYPES(INNER_GENERATE_TEXTURE_TYPE_SAMPLE, TextureType,       \
                        CoordType, OffsetType)                                 \
  ITERATE_TEXTURE_TYPES(INNER_GENERATE_TEXTURE_TYPE_SAMPLE_LEVEL, TextureType, \
                        CoordType, OffsetType)                                 \
  ITERATE_TEXTURE_TYPES(INNER_GENERATE_TEXTURE_TYPE_SAMPLE_GRAD, TextureType,  \
                        CoordType, OffsetType)                                 \
  INNER_GENERATE_TEXTURE_TYPE_FETCH_WITHOUT_TEMPLATE(TextureType)              \
  INNER_GENERATE_TEXTURE_TYPE_SAMPLE_WITHOUT_TEMPLATE(TextureType, CoordType,  \
                                                      OffsetType)              \
  INNER_GENERATE_TEXTURE_TYPE_SAMPLE_LEVEL_WITHOUT_TEMPLATE(                   \
      TextureType, CoordType, OffsetType)                                      \
  INNER_GENERATE_TEXTURE_TYPE_SAMPLE_GRAD_WITHOUT_TEMPLATE(                    \
      TextureType, CoordType, OffsetType)

#define INNER_GENERATE_BUFFER_FETCH()                                          \
  ByteAddressBuffer operator[](ByteBufferHandle handle) {                      \
    uint array_handle =                                                        \
        g__array_114514_bdls[NonUniformResourceIndex(handle.internalIndex)];   \
    return ByteAddressBuffer(                                                  \
        gbuffer__114514_bdls[NonUniformResourceIndex(array_handle)]);          \
  }

#define INNER_SAMPLER_FETCH()                                                  \
  SamplerState operator[](SamplerHeapHandle handle) {                          \
    return gsampler__114514_bdls[NonUniformResourceIndex(                      \
        handle.internalIndex)];                                                \
  }
#if VULKAN
#define VK_DESCRIPTOR_HEAP(INNER_GENERATE_TEXTURE_TYPE_FETCH,                  \
                           INNER_GENERATE_BUFFER_FETCH)                        \
  struct VKResourceDescriptorHeap {                                            \
    DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(Texture1D, float, int)               \
    DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(Texture2D, float2, int2)             \
    DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(Texture3D, float3, int3)             \
    INNER_GENERATE_BUFFER_FETCH()                                              \
    INNER_SAMPLER_FETCH()                                                      \
  };                                                                           \
  static VKResourceDescriptorHeap vkResourceDescriptorHeap;

#define DX_DESCRIPTOR_HEAP(INNER_GENERATE_TEXTURE_TYPE_FETCH,                  \
                           INNER_GENERATE_BUFFER_FETCH)
// struct VKResourceDescriptorHeap {
//   INNER_GENERATE_BUFFER_FETCH(BINDLESS_NAME_SUFFIX, BINDLESS_SUFFIX)
//   DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(Texture1D, float, int)
//   DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(Texture2D, float2, int2)
//   DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(Texture3D, float3, int3)
// };

#define DESCRIPTOR_HEAP_UNIFORM(HandleType, handle)                            \
  vkResourceDescriptorHeap[(HandleType)handle]

#define DESCRIPTOR_HEAP(HandleType, handle)                                    \
  vkResourceDescriptorHeap[(HandleType)handle]
#define DESCRIPTOR_HEAP_SAMPLE(HandleType, handle, uv, offset)                 \
  vkResourceDescriptorHeap.Sample(HandleType(handle), uv, offset)
#define DESCRIPTOR_HEAP_SAMPLE_LEVEL(HandleType, handle, uv, offset, level)    \
  vkResourceDescriptorHeap.SampleLevel(HandleType(handle), uv, offset, level)

#define DESCRIPTOR_HEAP_SAMPLE_GRAD(HandleType, handle, uv, offset, grad_x,    \
                                    grad_y)                                    \
  vkResourceDescriptorHeap.SampleGrad(HandleType(handle), uv, offset, grad_x,  \
                                      grad_y)

#elif DXIL

#define DESCRIPTOR_HEAP_UNIFORM(HandleType, handle)                            \
  ResourceDescriptorHeap[NonUniformResourceIndex(handle.internalIndex)]

#define DESCRIPTOR_HEAP(HandleType, handle)                                    \
  ResourceDescriptorHeap[(handle.internalIndex)]
#endif
// define resources
#define HANDLES(DESCRIPTOR_HEAP, DESCRIPTOR_HEAP_SAMPLE)                       \
  struct ArrayBuffer {                                                         \
    uint handle;                                                               \
    ByteAddressBuffer GetByteAddressBuffer() {                                 \
      return DESCRIPTOR_HEAP(ByteBufferHandle, handle);                        \
    }                                                                          \
    template <typename ReadStructure> ReadStructure Load(uint index) {         \
      return DESCRIPTOR_HEAP(ByteBufferHandle, handle)                         \
          .Load<ReadStructure>(sizeof(ReadStructure) * index);                 \
    }                                                                          \
    template <typename ReadStructure>                                          \
    ReadStructure Load(uint index, uint offset) {                              \
      return DESCRIPTOR_HEAP(ByteBufferHandle, handle)                         \
          .Load<ReadStructure>(sizeof(ReadStructure) * index + offset);        \
    }                                                                          \
  };                                                                           \
  struct TextureHandle {                                                       \
    uint handle;                                                               \
    template <typename TextureValue> TextureValue Sample2D(float2 uv) {        \
      int2 offset = int2(0, 0);                                                \
      return DESCRIPTOR_HEAP_SAMPLE(Texture2DSampleHandle<TextureValue>,       \
                                    handle, uv, offset);                       \
    }                                                                          \
    template <typename TextureValue>                                           \
    TextureValue SampleLevel(float2 uv, float level = 0.f) {                   \
      int2 offset = int2(0, 0);                                                \
      return DESCRIPTOR_HEAP_SAMPLE_LEVEL(Texture2DSampleHandle<TextureValue>, \
                                          handle, uv, offset, level);          \
    }                                                                          \
    float4 SampleLevel(float2 uv, float level) {                               \
      int2 offset = int2(0, 0);                                                \
      return DESCRIPTOR_HEAP_SAMPLE_LEVEL(Texture2DSampleHandleNative, handle, uv,  \
                                     offset, level);                           \
    }                                                                          \
    template <typename TextureValue>                                           \
    TextureValue SampleGrad(float2 uv, float2 grad_x, float2 grad_y) {         \
      int2 offset = int2(0, 0);                                                \
      return DESCRIPTOR_HEAP_SAMPLE_GRAD(Texture2DSampleHandle<TextureValue>,  \
                                         handle, uv, offset, grad_x, grad_y);  \
    }                                                                          \
    template <typename TextureValue> Texture2D<TextureValue> GetTexture2D() {  \
      return DESCRIPTOR_HEAP(Texture2DHandle<TextureValue>, handle);           \
    }                                                                          \
    Texture2D GetTexture2D() {                                                 \
      return DESCRIPTOR_HEAP(Texture2DHandleNative, handle);                   \
    }                                                                          \
  };                                                                           \
  struct SamplerHandle {                                                       \
    uint handle;                                                               \
    SamplerState GetSampler() {                                                \
      return DESCRIPTOR_HEAP(SamplerHeapHandle, handle);                       \
    }                                                                          \
  };

#define BINDLESS_ACCEL(Space)                                                  \
  [[vk::binding(                                                               \
      0, Space)]] RaytracingAccelerationStructure gaccelg__114514_bdls[];

#define BINDLESS_BINDINGS(BufferSpace, TextureSpace, SamplerSpace, AccelSpace) \
  [[vk::binding(0, BufferSpace)]] StructuredBuffer<uint> g__array_114514_bdls; \
  [[vk::binding(1, BufferSpace)]] ByteAddressBuffer gbuffer__114514_bdls[];    \
  [[vk::binding(0, SamplerSpace)]] SamplerState gsampler__114514_bdls[];       \
  DEFINE_TEXTURE_TYPE_AND_FORMATS_SLOTS(Texture1D, 0, TextureSpace)            \
  DEFINE_TEXTURE_TYPE_AND_FORMATS_SLOTS(Texture2D, 0, TextureSpace)            \
  DEFINE_TEXTURE_TYPE_AND_FORMATS_SLOTS(Texture3D, 0, TextureSpace)            \
  DEFINE_TEXTURE_TYPE_AND_FORMATS_SLOTS(TextureCube, 0, TextureSpace)          \
  BINDLESS_ACCEL(AccelSpace)                                                   \
  VK_DESCRIPTOR_HEAP(INNER_GENERATE_TEXTURE_TYPE_FETCH,                        \
                     INNER_GENERATE_BUFFER_FETCH)                              \
  DX_DESCRIPTOR_HEAP(INNER_GENERATE_TEXTURE_TYPE_FETCH,                        \
                     INNER_GENERATE_BUFFER_FETCH)                              \
  HANDLES(DESCRIPTOR_HEAP, DESCRIPTOR_HEAP_SAMPLE)

#endif