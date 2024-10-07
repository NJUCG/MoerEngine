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
                        Binding, Set)

struct ByteBufferHandle {
  uint internalIndex;
};

struct TlasHandle {
  uint internalIndex;
};
template <typename T> struct Texture1DHandle {
  uint internalIndex;
};

template <typename T> struct Texture2DHandle {
  uint internalIndex;
};

template <typename T> struct Texture3DHandle {
  uint internalIndex;
};

template <typename T> struct Texture1DSampleHandle {
  uint internalIndex;
};

template <typename T> struct Texture2DSampleHandle {
  uint internalIndex;
};

template <typename T> struct Texture3DSampleHandle {
  uint internalIndex;
};

#define INNER_GENERATE_TEXTURE_TYPE_FETCH(NativeType, TextureType)             \
  TextureType<NativeType> operator[](TextureType##Handle<NativeType> handle) { \
    uint tex_handle =                                                          \
        g__array_114514_bdls[NonUniformResourceIndex(handle.internalIndex)];   \
    uint tex_idx = tex_handle >> 8;                                            \
    return TextureType<NativeType>(                                            \
        g##TextureType##NativeType##__114514_bdls[NonUniformResourceIndex(     \
            tex_handle)]);                                                     \
  }

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

#define DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(TextureType, CoordType,          \
                                              OffsetType)                      \
  ITERATE_TEXTURE_TYPES(INNER_GENERATE_TEXTURE_TYPE_FETCH, TextureType)        \
  ITERATE_TEXTURE_TYPES(INNER_GENERATE_TEXTURE_TYPE_SAMPLE, TextureType,       \
                        CoordType, OffsetType)

#define INNER_GENERATE_BUFFER_FETCH()                                          \
  ByteAddressBuffer operator[](ByteBufferHandle handle) {                      \
    uint array_handle =                                                        \
        g__array_114514_bdls[NonUniformResourceIndex(handle.internalIndex)];   \
    return ByteAddressBuffer(                                                  \
        gbuffer__114514_bdls[NonUniformResourceIndex(array_handle)]);          \
  }
#if VULKAN
#define VK_DESCRIPTOR_HEAP(INNER_GENERATE_TEXTURE_TYPE_FETCH,                  \
                           INNER_GENERATE_BUFFER_FETCH)                        \
  struct VKResourceDescriptorHeap {                                            \
    DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(Texture1D, float, int)               \
    DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(Texture2D, float2, int2)             \
    DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(Texture3D, float3, int3)             \
    INNER_GENERATE_BUFFER_FETCH()                                              \
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