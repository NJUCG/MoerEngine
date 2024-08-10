#ifndef FRAMEWORK_BINDLESS_COMMON_HLSL
#define FRAMEWORK_BINDLESS_COMMON_HLSL

#define DEBUG_MODE 1
#define NUM_STATIC_SAMPLERS 256
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
#if VULKAN_HLSL
  uint WriteIndex() { return ReadIndex(); }
#else
  uint WriteIndex() { return ReadIndex() + 1; }
#endif
  void LogInfo() {
    printf("[hlsl] index: %d, ResourceTag: %d, IsWritable: %d, Version: %d, "
           "ReadIndex: %d, WriteIndex: %d\n",
           index, ResourceTag(), IsWritable() ? 1 : 0,
           Version(), ReadIndex(), WriteIndex());
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

#define INNER_GENERATE_TEXTURE_TYPE_SLOT(NativeType, TextureType, Name, Binding,     \
                                         Set)                                  \
  [[vk::binding(Binding, Set)]] TextureType<NativeType>                        \
      g_##Name##NativeType[BINDLESS_DESCRIPTOR_HEAP_SIZE];

#define DEFINE_TEXTURE_TYPE_AND_FORMATS_SLOTS(TextureType, Name, Binding, Set)       \
  ITERATE_TEXTURE_TYPES(INNER_GENERATE_TEXTURE_TYPE_SLOT, TextureType, Name         \
                        Binding, Set)

#define BINDLESS_TEXTURE(Name, Space) \
  [[vk::binding(0,
              Space)]] SamplerState g_samplerState[NUM_STATIC_SAMPLERS]; \
  DEFINE_TEXTURE_TYPE_AND_FORMATS_SLOTS(Texture1D, Name, NUM_STATIC_SAMPLERS, Space)\
  DEFINE_TEXTURE_TYPE_AND_FORMATS_SLOTS(Texture2D, Name, NUM_STATIC_SAMPLERS, Space)\
  DEFINE_TEXTURE_TYPE_AND_FORMATS_SLOTS(Texture3D, Name, NUM_STATIC_SAMPLERS, Space)\
  DEFINE_TEXTURE_TYPE_AND_FORMATS_SLOTS(TextureCube, Name, NUM_STATIC_SAMPLERS, Space)

#define BINDLESS_BUFFER(Name, Space) \
  [[vk::binding(0, Space)]] ByteAddressBuffer g_##Name[];

#define BINDLESS_ACCEL(Name, Space) \
  [[vk::binding(0, Space)]] RaytracingAccelerationStructure g_##Name[];
// DEFINE_TEXTURE_TYPE_AND_FORMATS_SLOTS(Texture1D, NUM_STATIC_SAMPLERS,
//                                       TEXTURE_SET)
// DEFINE_TEXTURE_TYPE_AND_FORMATS_SLOTS(Texture2D, NUM_STATIC_SAMPLERS,
//                                       TEXTURE_SET)
// DEFINE_TEXTURE_TYPE_AND_FORMATS_SLOTS(Texture3D, NUM_STATIC_SAMPLERS,
//                                       TEXTURE_SET)
// DEFINE_TEXTURE_TYPE_AND_FORMATS_SLOTS(TextureCube, NUM_STATIC_SAMPLERS,
//                                       TEXTURE_SET)

// DEFINE_TEXTURE_TYPE_AND_FORMATS_SLOTS(RWTexture1D, 0, RWTEXTURE_SET)

// DEFINE_TEXTURE_TYPE_AND_FORMATS_SLOTS(RWTexture2D, 0, RWTEXTURE_SET)

// DEFINE_TEXTURE_TYPE_AND_FORMATS_SLOTS(RWTexture3D, 0, RWTEXTURE_SET)


// [[vk::binding(0, BUFFER_SET)]] ByteAddressBuffer g_byteAddressBuffer[];
// [[vk::binding(0, BUFFER_SET)]] RWByteAddressBuffer g_rwByteAddressBuffer[];

// [[vk::binding(
//     0, ACCELERATION_STRUCTURE_SET)]] RaytracingAccelerationStructure g_tlas[];

// SamplerState SamplerMinMagMipPointWrap() { return g_samplerState[0]; }

struct ByteBufferHandle {
  uint internalIndex;
};
struct RWByteBufferHandle {
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

template <typename T> struct RWTexture1DHandle {
  uint internalIndex;
};

template <typename T> struct RWTexture2DHandle {
  uint internalIndex;
};

template <typename T> struct RWTexture3DHandle {
  uint internalIndex;
};

#define INNER_GENERATE_TEXTURE_TYPE_FETCH(NativeType, TextureType)             \
  TextureType<NativeType> operator[](TextureType##Handle<NativeType> handle) { \
    return TextureType<NativeType>(                                            \
        g_##TextureType##NativeType[NonUniformResourceIndex(                   \
            handle.internalIndex)]);                                           \
  }

#define DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(TextureType)                     \
  ITERATE_TEXTURE_TYPES(INNER_GENERATE_TEXTURE_TYPE_FETCH, TextureType)

#if VULKAN_HLSL  
struct VKResourceDescriptorHeap {
  ByteAddressBuffer operator[](ByteBufferHandle handle) {
    return ByteAddressBuffer(
      
        g_byteAddressBuffer[NonUniformResourceIndex(handle.internalIndex)]);
  }
  RWByteAddressBuffer operator[](RWByteBufferHandle handle) {
    return RWByteAddressBuffer(
        g_rwByteAddressBuffer[NonUniformResourceIndex(handle.internalIndex)]);
  }

  RaytracingAccelerationStructure operator[](TlasHandle handle) {
    return g_tlas[NonUniformResourceIndex(handle.internalIndex)];
  }

  DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(Texture1D)
  DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(Texture2D)
  DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(Texture3D)
  DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(RWTexture1D)
  DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(RWTexture2D)
  DEFINE_FETCH_TEXTURE_TYPE_AND_FORMATS(RWTexture3D)
};

static VKResourceDescriptorHeap vkResourceDescriptorHeap;

#define DESCRIPTOR_HEAP_UNIFORM(HandleType, handle)                            \
  vkResourceDescriptorHeap[(HandleType)handle]

#define DESCRIPTOR_HEAP(HandleType, handle)                                    \
  vkResourceDescriptorHeap[(HandleType)handle]

#elif DXIL

#define DESCRIPTOR_HEAP_UNIFORM(HandleType, handle)                            \
  ResourceDescriptorHeap[NonUniformResourceIndex(handle.internalIndex)]

#define DESCRIPTOR_HEAP(HandleType, handle)                                    \
    ResourceDescriptorHeap[(handle.internalIndex)]
#endif
// define resources

struct ArrayBuffer {
  RenderResourceHandle handle;

  ByteAddressBuffer GetByteAddressBuffer() {
    return DESCRIPTOR_HEAP(ByteBufferHandle, handle.ReadIndex());
  }

  template <typename ReadStructure> ReadStructure Load(uint index) {
    return DESCRIPTOR_HEAP(ByteBufferHandle, handle.ReadIndex())
        .Load<ReadStructure>(sizeof(ReadStructure) * index);
  }

  template <typename ReadStructure> ReadStructure LoadUniform(uint index) {
    // UNIFORM access here means that we’re internally not accessing the
    // descriptorheap with NURI.
    ByteAddressBuffer buffer =
        DESCRIPTOR_HEAP_UNIFORM(ByteBufferHandle, handle.ReadIndex());
    ReadStructure result =
        buffer.Load<ReadStructure>(sizeof(ReadStructure) * index);
    return result;
  }
};

struct RWArrayBuffer {
  RenderResourceHandle handle;

  RWByteAddressBuffer GetRWByteAddressBuffer() {
    return DESCRIPTOR_HEAP(RWByteBufferHandle, handle.WriteIndex());
  }

  template <typename ReadStructure> ReadStructure Load(uint index) {
    ByteAddressBuffer buffer =
        DESCRIPTOR_HEAP(ByteBufferHandle, handle.WriteIndex());
    ReadStructure result =
        buffer.Load<ReadStructure>(sizeof(ReadStructure) * index);
    return result;
  }

  template <typename ReadStructure> ReadStructure LoadUniform(uint index) {
    // UNIFORM access here means that we’re internally not accessing the
    // descriptorheap with NURI.
    ByteAddressBuffer buffer =
        DESCRIPTOR_HEAP_UNIFORM(ByteBufferHandle, handle.WriteIndex());
    ReadStructure result =
        buffer.Load<ReadStructure>(sizeof(ReadStructure) * index);
    return result;
  }

  template <typename WriteStructure>
  void Store(uint index, WriteStructure value) {

    DESCRIPTOR_HEAP(RWByteBufferHandle, handle.WriteIndex())
        .Store<WriteStructure>(sizeof(WriteStructure) * index, value);
  }
  // template <typename WriteStructure>
  // void operator[](in uint index, in WriteStructure value) {
  //   RWByteAddressBuffer buffer =
  //       DESCRIPTOR_HEAP(RWByteBufferHandle, handle.WriteIndex());
  //   buffer.Store<WriteStructure>(sizeof(WriteStructure) * index, value);
  // }
};

struct Texture {
  RenderResourceHandle handle;

  template <typename TextureValue> Texture1D<TextureValue> GetTexture1D() {
    Texture1D<TextureValue> texture =
        DESCRIPTOR_HEAP(Texture1DHandle<TextureValue>, handle.ReadIndex());
    return texture;
  }

  template <typename TextureValue> Texture2D<TextureValue> GetTexture2D() {
    Texture2D<TextureValue> texture =
        DESCRIPTOR_HEAP(Texture2DHandle<TextureValue>, handle.ReadIndex());
    return texture;
  }

  template <typename TextureValue> Texture3D<TextureValue> GetTexture3D() {
    Texture3D<TextureValue> texture =
        DESCRIPTOR_HEAP(Texture3DHandle<TextureValue>, handle.ReadIndex());
    return texture;
  }

  template <typename TextureValue> TextureValue Load1D(uint pos) {
    Texture1D<TextureValue> texture =
        DESCRIPTOR_HEAP(Texture1DHandle<TextureValue>, handle.ReadIndex());
    return texture.load(pos);
  }

  template <typename TextureValue> TextureValue Load2D(uint2 pos) {
    Texture2D<TextureValue> texture =
        DESCRIPTOR_HEAP(Texture2DHandle<TextureValue>, handle.ReadIndex());
    return texture.load(uint2(pos));
  }

  template <typename TextureValue> TextureValue Load3D(uint3 pos) {
    Texture3D<TextureValue> texture =
        DESCRIPTOR_HEAP(Texture3DHandle<TextureValue>, handle.ReadIndex());
    return texture.load(uint3(pos));
  }

  template <typename TextureValue>
  TextureValue SampleLevel2D(SamplerState s, float2 uv, float mip) {
    Texture2D<TextureValue> texture =
        DESCRIPTOR_HEAP(Texture2DHandle<TextureValue>, handle.ReadIndex());
    return texture.SampleLevel(s, uv, mip);
  }

  template <typename TextureValue>
  TextureValue SampleLevel3D(SamplerState s, float3 uv, float mip) {
    Texture3D<TextureValue> texture =
        DESCRIPTOR_HEAP(Texture3DHandle<TextureValue>, handle.ReadIndex());
    return texture.SampleLevel(s, uv, mip);
  }
};

struct RWTexture {
  RenderResourceHandle handle;
  // This should also implements load functions

  template <typename RWTextureValue>
  RWTexture1D<RWTextureValue> GetRWTexture1D() {
    RWTexture1D<RWTextureValue> texture = DESCRIPTOR_HEAP(
        RWTexture1DHandle<RWTextureValue>, handle.WriteIndex());
    return texture;
  }

  template <typename RWTextureValue>
  RWTexture2D<RWTextureValue> GetRWTexture2D() {
    RWTexture2D<RWTextureValue> texture = DESCRIPTOR_HEAP(
        RWTexture2DHandle<RWTextureValue>, handle.WriteIndex());
    return texture;
  }

  template <typename RWTextureValue>
  RWTexture3D<RWTextureValue> GetRWTexture3D() {
    RWTexture3D<RWTextureValue> texture = DESCRIPTOR_HEAP(
        RWTexture3DHandle<RWTextureValue>, handle.WriteIndex());
    return texture;
  }

  template <typename RWTextureValue> RWTextureValue Load2D(uint2 pos) {
    RWTexture2D<RWTextureValue> texture = DESCRIPTOR_HEAP(
        RWTexture2DHandle<RWTextureValue>, handle.WriteIndex());
    return texture[pos];
  }

  template <typename RWTextureValue> RWTextureValue Load3D(uint3 pos) {
    RWTexture3D<RWTextureValue> texture = DESCRIPTOR_HEAP(
        RWTexture3DHandle<RWTextureValue>, handle.WriteIndex());
    return texture[pos];
  }

  template <typename RWTextureValue>
  void Store2D(uint2 pos, RWTextureValue value) {
    RWTexture2D<RWTextureValue> texture = DESCRIPTOR_HEAP(
        RWTexture2DHandle<RWTextureValue>, handle.WriteIndex());
    texture[pos] = value;
  }

  template <typename RWTextureValue>
  void Store3D(uint3 pos, RWTextureValue value) {
    RWTexture3D<RWTextureValue> texture = DESCRIPTOR_HEAP(
        RWTexture3DHandle<RWTextureValue>, handle.WriteIndex());
    texture[pos] = value;
  }
};

struct TypeBuffer {
  RenderResourceHandle handle;

  ByteAddressBuffer GetByteAddressBuffer() {
    ByteAddressBuffer buffer =
        DESCRIPTOR_HEAP(ByteBufferHandle, handle.ReadIndex());
    return buffer;
  }

  template <typename Type> Type Load() {
    ByteAddressBuffer buffer =
        DESCRIPTOR_HEAP(ByteBufferHandle, handle.ReadIndex());
    Type result = buffer.Load<Type>(0);
    return result;
  }
};

struct RWTypeBuffer {
  RenderResourceHandle handle;

  template <typename Type> RWByteAddressBuffer GetRWByteAddressBuffer() {
    RWByteAddressBuffer buffer =
        DESCRIPTOR_HEAP(RWByteBufferHandle, handle.WriteIndex());
    return buffer;
  }

  template <typename Type> Type Load() {
    RWByteAddressBuffer buffer =
        DESCRIPTOR_HEAP(RWByteBufferHandle, handle.WriteIndex());
    Type result = buffer.Load<Type>(0);
    return result;
  }
  template <typename Type> void Store(Type value) {
    RWByteAddressBuffer buffer =
        DESCRIPTOR_HEAP(RWByteBufferHandle, handle.WriteIndex());
    buffer.Store<Type>(0);
  }
};

struct Tlas {
  RenderResourceHandle handle;
  RaytracingAccelerationStructure GetAccelerationStructure() {
    RaytracingAccelerationStructure result =
        DESCRIPTOR_HEAP(TlasHandle, handle.ReadIndex());
    return result;
  }
};

struct MaterialData {
  // Texture base_color_texture_handle;
  // Texture specular_color_texture_handle;
  // Texture metallic_roughness_texture_handle;
  // Texture roughness_texture_handle;

  // Texture normal_texture_handle;
  // Texture emissive_color_texture_handle;
  // uint base_color_texture_offset;
  // uint user_data2;

  float4 baseColorFactor;
  float4 specularColorFactor;

  float metallicFactor;
  float roughnessFactor;
  float normalScale;
  float occlusionStrength;

  float3 emissiveFactor;
  uint materialType;

  uint is_blend;
  uint is_double_sided;
  uint has_normal;
  float eta;

  uint base_color_texture_offset;
  uint user_data0;
  uint user_data1;
  uint user_data2;

  template <typename TextureValue>
  Texture2D<TextureValue> GetBaseColorTexture() {
    return DESCRIPTOR_HEAP(Texture2DHandle<TextureValue>,
                           base_color_texture_offset);
  }

  template <typename TextureValue>
  Texture2D<TextureValue> GetSpecularColorTexture() {
    return DESCRIPTOR_HEAP(Texture2DHandle<TextureValue>,
                           (base_color_texture_offset + 1));
  }

  template <typename TextureValue>
  Texture2D<TextureValue> GetMetallicRoughnessTexture() {
    return DESCRIPTOR_HEAP(Texture2DHandle<TextureValue>,
                           (base_color_texture_offset + 2));
  }

  template <typename TextureValue>
  Texture2D<TextureValue> GetRoughnessTexture() {
    return DESCRIPTOR_HEAP(Texture2DHandle<TextureValue>,
                           (base_color_texture_offset + 3));
  }

  template <typename TextureValue> Texture2D<TextureValue> GetNormalTexture() {
    return DESCRIPTOR_HEAP(Texture2DHandle<TextureValue>,
                           (base_color_texture_offset + 4));
  }

  template <typename TextureValue>
  Texture2D<TextureValue> GetEmissiveColorTexture() {
    return DESCRIPTOR_HEAP(Texture2DHandle<TextureValue>,
                           (base_color_texture_offset + 5));
  }
};

#endif