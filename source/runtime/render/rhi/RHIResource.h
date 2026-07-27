#ifndef RHI_RESOURCE_H
#define RHI_RESOURCE_H
#include "PixelFormat.h"
#include "RHICommon.h"
#include "math/Base.h"

#include "math/Matrix.h"
#include "misc/CountableRef.h"
#include "misc/Hash.h"

#include "misc/STL.h"
#include "misc/Traits.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResourceInitilizer.h"

#include <atomic>
#include <cassert>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdint.h>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

static constexpr std::string_view default_name = "NoName";
template<typename TStructuredParam>
concept concept_is_shader_struct = requires(TStructuredParam t) {
    std::is_same<typename TStructuredParam::TypeInfo::TParamPtr, TStructuredParam>();
    t.GetStructMetadata();
};
// #pragma region forward definitions
// class RHICommandListBase;
// class RHITexture;
// class RHIAmplificationShader;
// class RHIBlendState;
// class RHIShaderBoundStateInput;
// class RHIBuffer;
// class RHIGfxPso;
// class RHIComputePso;
// class RHIComputeShader;
// class RHIDepthStencilState;
// class RHIGeometryShader;
// class RHIFence;
// class RHIGraphicsPipelineState;
// class RHIMeshShader;
// class RHIPipelineBinaryDataLibrary;
// class RHIFragmentShader;
// class RHIRasterizationState;
// class RHIRTPso;
// class RHIRayTracingAccelerationStructure;
// class RHIRayTracingBLAS;
// class RHIRayTracingTLAS;
// class RHIRayTracingShader;
// class RHIRayGenShader;
// class RHIRayMissShader;
// class RHIRayClosestHitShader;
// class RHIRayCallableShader;
// class RHIRayIntersectionShader;
// class RHIRayAnyhitShader;
// class RHIRenderQuery;
// class RHIRenderQueryPool;
// class RHIResource;
// class RHISampler;
// class RHIMultisampleState;
// class RHIShader;
// class RHIShaderLibrary;
// class RHISRV;
// class RHICBV;
// class RHIView;
// class RHIConstantBufferView;
// class RHITexture;
// class RHITextureReference;
// class RHIShaderRootParameterLayout;
// class RHIUAV;
// class RHIVertexInputState;
// class RHIVertexShader;
// class RHIViewableResource;
// class RHIViewport;

// template<concept_is_shader_struct TStructuredType>
// class RHIStructuredBuffer;

// using RHIAmplificationShaderRef = CountableRef<RHIAmplificationShader>;
// using RHIBlendStateRef          = CountableRef<RHIBlendState>;
// using RHIShaderBoundStateRef    = CountableRef<RHIShaderBoundStateInput>;
// using RHIBufferRef              = CountableRef<RHIBuffer>;

// template<concept_is_shader_struct TStructuredType>
// using RHIStructuredBufferRef          = CountableRef<RHIStructuredBuffer<TStructuredType>>;
// using RHIComputePsoRef                = CountableRef<RHIComputePso>;
// using RHIComputeShaderRef             = CountableRef<RHIComputeShader>;
// using RHIDepthStencilStateRef         = CountableRef<RHIDepthStencilState>;
// using RHIGeometryShaderRef            = CountableRef<RHIGeometryShader>;
// using RHIFenceRef                     = CountableRef<RHIFence>;
// using RHIGfxPsoRef                    = CountableRef<RHIGfxPso>;
// using RHIMeshShaderRef                = CountableRef<RHIMeshShader>;
// using RHIPipelineBinaryDataLibraryRef = CountableRef<RHIPipelineBinaryDataLibrary>;
// using RHIFragmentShaderRef            = CountableRef<RHIFragmentShader>;
// using RHIRasterizationStateRef        = CountableRef<RHIRasterizationState>;
// using RHIRayTracingBLASRef            = CountableRef<RHIRayTracingBLAS>;
// using RHIRayTracingTLASRef            = CountableRef<RHIRayTracingTLAS>;
// using RHIRTPsoRef                     = CountableRef<RHIRTPso>;
// using RHIRayTracingShaderRef          = CountableRef<RHIRayTracingShader>;
// using RHIRayGenShaderRef              = CountableRef<RHIRayGenShader>;
// using RHIRayMissShaderRef             = CountableRef<RHIRayMissShader>;
// using RHIRayClosestHitShaderRef       = CountableRef<RHIRayClosestHitShader>;
// using RHIRayIntersectionShaderRef     = CountableRef<RHIRayIntersectionShader>;
// using RHIRayAnyhitShaderRef           = CountableRef<RHIRayAnyhitShader>;
// using RHIRayCallableShaderRef         = CountableRef<RHIRayCallableShader>;
// using RHIRenderQueryRef               = CountableRef<RHIRenderQuery>;
// using RHIRenderQueryPoolRef           = CountableRef<RHIRenderQueryPool>;
// using RHIResourceRef                  = CountableRef<RHIResource>;
// using RHISamplerRef                   = CountableRef<RHISampler>;
// using RHIMultisampleStateRef          = CountableRef<RHIMultisampleState>;
// using RHIShaderRef                    = CountableRef<RHIShader>;
// using RHIShaderLibraryRef             = CountableRef<RHIShaderLibrary>;
// using RHISRVRef                       = CountableRef<RHISRV>;
// using RHICBVRef                       = CountableRef<RHICBV>;
// using RHIViewRef                      = CountableRef<RHIView>;
// using RHITextureRef                   = CountableRef<RHITexture>;
// using RHITextureReferenceRef          = CountableRef<RHITextureReference>;
// using RHIShaderRootParameterLayoutRef = CountableRef<RHIShaderRootParameterLayout>;
// using RHIUAVRef                       = CountableRef<RHIUAV>;
// using RHIVertexInputStateRef          = CountableRef<RHIVertexInputState>;
// using RHIVertexShaderRef              = CountableRef<RHIVertexShader>;
// using RHIViewableResourceRef          = CountableRef<RHIViewableResource>;
// using RHIViewportRef                  = CountableRef<RHIViewport>;
// #pragma endregion

namespace Moer::Render {
class Texture;
class Buffer;
class Fence;
class Sampler;
class DepthBuffer;
class Swapchain;
class BindlessArray;
class RaytracingGeometry;
class RaytracingScene;
class RaytracingTlas;
using TextureRef            = CountableRef<Texture>;
using BufferRef             = CountableRef<Buffer>;
using FenceRef              = CountableRef<Fence>;
using SamplerRef            = CountableRef<Sampler>;
using DepthBufferRef        = CountableRef<DepthBuffer>;
using SwapchainRef          = CountableRef<Swapchain>;
using BindlessArrayRef      = CountableRef<BindlessArray>;
using RaytracingGeometryRef = CountableRef<RaytracingGeometry>;
using RaytracingSceneRef    = CountableRef<RaytracingScene>;
using RaytracingTlasRef     = CountableRef<RaytracingTlas>;

struct TextureWithHandle {
    TextureRef  tex;
    uint        hdl;         //主Handle
    Array<uint> mip_handles; //每个Mip的Handle

    uint2  GetSize(uint mip = 0) const;
    uint   GetSizeX(uint mip = 0) const;
    uint   GetSizeY(uint mip = 0) const;
    Rect2D GetRect2D(uint mip = 0) const;
    uint   GetMipHandle(uint mip) const;
};

struct DepthBufferWithHandle {
    DepthBufferRef tex;
    uint           hdl = 0;

    uint2 GetSize();
};

struct BufferWithHandle {
    BufferRef buf;
    uint      hdl = 0;
};

// 如果texture的名字不是编译期决定的，则需要找一个地方存名字。否则string_view会出现悬垂指针
struct DepthBufferWithHandleAndName {
    DepthBufferRef tex;
    uint           hdl = 0;
    std::string    name;
};

}; // namespace Moer::Render

class Shader;
class RHITextureBarrierInfo;

#pragma region utils definition

struct VertexAttrb {
    EPixelFormat format;
};

struct VertexBinding {
    EVertexInputRate input_rate = EVertexInputRate::VIR_VERTEX;

    Moer::Array<VertexAttrb> vertex_elements;

    VertexBinding() = default;

    template<class... _Valty>
        requires(std::is_same_v<std::remove_cvref_t<_Valty>, VertexAttrb> && ...)
    VertexBinding(_Valty&&... _Val) : vertex_elements({std::forward<_Valty>(_Val)...}) {}
};

struct VertexElement {
    VertexElement() = default;

    VertexElement(
        uint8_t          _binding_index,
        uint8_t          _offset,
        EPixelFormat     _format,
        uint8_t          _attribute_index,
        uint16_t         _stride,
        EVertexInputRate _input_rate
    ) :
        binding_index(_binding_index),
        offset(_offset),
        format(_format),
        attribute_index(_attribute_index),
        stride(_stride),
        input_rate(_input_rate) {}

    bool operator==(const VertexElement& other) const {
        return binding_index == other.binding_index && offset == other.offset && format == other.format &&
               attribute_index == other.attribute_index && stride == other.stride &&
               input_rate == other.input_rate;
    }
    uint8_t                      binding_index;
    uint8_t                      offset;
    EnumInByte<EPixelFormat>     format;
    uint8_t                      attribute_index;
    uint16_t                     stride;
    EnumInByte<EVertexInputRate> input_rate;
    uint8_t                      reserve_byte;
};

static_assert(sizeof(VertexElement) == 8, "VertexElement doesn't match cache line size");

#pragma endregion

class RENDER_API RHIResource {
public:
    explicit RHIResource(ERHIResourceType _type = ERHIResourceType::RRT_NONE) : type(_type) {}
    virtual ~RHIResource() = default;

public:
    uint32_t AddRef() {
        //first AddRef() happens before DeRef
        int32_t ref_count = flags.AddRef(std::memory_order_acquire);
        assert(ref_count > 0);
        return ref_count;
    };

    uint32_t DeRef() {
        int32_t ref_count = flags.DeRef(std::memory_order_release);
        assert(ref_count >= 0);
        if (ref_count == 0) {
            Destroy();
        }
        return (uint32_t)ref_count;
    };
    //only for look-up purposes, don't care about sequences
    uint32_t GetRefCount() const {
        return (uint32_t)flags.GetRefCount(std::memory_order_relaxed);
    }

    /**
     * Acquire-loads the intrusive count before reporting sole ownership.
     *
     * This is the synchronization query for pools that reuse a resource after
     * another thread drops its last external reference with DeRef(release).
     */
    [[nodiscard]] bool IsUniquelyReferenced() const {
        return flags.GetRefCount(std::memory_order_acquire) == 1;
    }

    bool IsValid() const {
        return flags.IsValid(std::memory_order_relaxed);
    }

    void Delete() {
        if (flags.MarkToDelete(std::memory_order_acquire)) {
            // delete this;
        }
    }

    ERHIResourceType GetResourceType() const {
        return type;
    }

protected:
    virtual void Destroy();

private:
    struct ResourceAtomicFlags {
        std::atomic<uint32_t> packed;

        static constexpr uint32_t s_mark_for_delete_mask = 1 << 31;
        static constexpr uint32_t s_is_deleting_mask     = 1 << 30;
        static constexpr uint32_t s_ref_count_mask       = s_is_deleting_mask - 1;

    public:
        int32_t AddRef(std::memory_order memory_order) {
            uint32_t current_packed = packed.fetch_add(1, memory_order);
            assert((current_packed & s_is_deleting_mask) == 0 && "resource is deleting");
            int32_t num_ref = (current_packed & s_ref_count_mask) + 1;
            assert(num_ref < s_mark_for_delete_mask);
            return num_ref;
        }

        int32_t DeRef(std::memory_order memory_order) {
            uint32_t current_packed = packed.fetch_sub(1, memory_order);
            assert((current_packed & s_is_deleting_mask) == 0 && "resource is deleting");
            int32_t num_ref = (current_packed & s_ref_count_mask) - 1;
            assert(num_ref >= 0);
            return num_ref;
        }

        bool MarkToDelete(std::memory_order memory_order) {
            uint32_t current_packed = packed.fetch_or(s_mark_for_delete_mask, memory_order);
            assert((current_packed & s_is_deleting_mask) == 0 && "resource is deleting");
            return (current_packed & s_mark_for_delete_mask) != 0;
        }

        bool UnMarkToDelete(std::memory_order memory_order) {
            uint32_t current_packed = packed.fetch_xor(s_mark_for_delete_mask, memory_order);
            assert((current_packed & s_is_deleting_mask) == 0 && "resource is deleting");
            bool current_mark_for_delete = (current_packed & s_mark_for_delete_mask) != 0;
            assert(current_mark_for_delete && "resource is not marked for deleting");
            return current_mark_for_delete;
        }

        bool IsDeleting() {
            /* make sure packed data processing sequence handled correctly - acquire-rel */
            uint32_t current_packed = packed.load(std::memory_order_acquire);
            assert((current_packed & s_mark_for_delete_mask) != 0 && "resource not marked for deleting");
            assert((current_packed & s_is_deleting_mask) != 0 && "resource is currently deleting");
            uint32_t num_ref = current_packed & s_ref_count_mask;
            if (num_ref == 0) {
                return true;
            }
            UnMarkToDelete(std::memory_order_release);
            return false;
        }

        bool IsValid(std::memory_order memory_order) {
            uint32_t current_packed = packed.load(memory_order);
            return (current_packed & s_mark_for_delete_mask) == 0 && (current_packed & s_ref_count_mask) > 0;
        }

        bool IsMarkedForDeleting(std::memory_order memory_order) {
            return (packed.load(memory_order) & s_mark_for_delete_mask) != 0;
        }
        int32_t GetRefCount(std::memory_order memory_order) {
            return packed.load(memory_order) & s_ref_count_mask;
        }
    };

    ERHIResourceType type;
    //for const resource state change
    mutable ResourceAtomicFlags flags;
};

#pragma region new api
namespace Moer::Render {

struct VertexElement {
    EPixelFormat format;
};

struct VertexBinding {
    VertexBinding() = default;
    VertexBinding(std::initializer_list<VertexElement> _elements, EVertexInputRate _vet) :
        vertex_elements(_elements),
        input_rate(_vet) {}
    VertexBinding(VertexElement&& element, EVertexInputRate _vet) :
        vertex_elements({std::move(element)}),
        input_rate(_vet) {}

    Moer::Array<VertexElement> vertex_elements;
    EVertexInputRate           input_rate = VIR_VERTEX;
};

struct VertexStream {
    Moer::Array<VertexBinding> bindings;
    void                       Emplace(VertexBinding&& _binding) {
        bindings.emplace_back(std::move(_binding));
    }
    void EmplacePerVertex(std::initializer_list<VertexElement> _elements) {
        bindings.emplace_back(_elements, VIR_VERTEX);
    }
    void EmplacePerInstance(std::initializer_list<VertexElement> _elements) {
        bindings.emplace_back(_elements, VIR_INSTANCE);
    }
};

struct Sampler {
    Sampler(
        ESamplerFilter          _filter,
        ESamplerAddressMode     _address_mode,
        ESamplerCompareFunction _compare_function = ESamplerCompareFunction::SCF_NEVER
    ) :
        filter(_filter),
        address_mode(_address_mode),
        compare_function(_compare_function) {}
    ESamplerFilter          filter;
    ESamplerAddressMode     address_mode;
    ESamplerCompareFunction compare_function;
};

class BufferView {
public:
    BufferView() = default;
    BufferView(Buffer* _buffer, EPixelFormat _fmt = PF_UNDEFINED);

    BufferView(
        Buffer*      _buffer,
        uint64       _byte_offset,
        uint64       _num_elements,
        uint         _stride,
        EPixelFormat _fmt = PF_UNDEFINED
    ) :
        buffer(_buffer),
        byte_offset(_byte_offset),
        num_elements(_num_elements),
        stride(_stride),
        format(_fmt) {};
    uint GetNumElements() const {
        return num_elements;
    }
    uint GetStride() const {
        return stride;
    }
    uint64 GetByteOffset() const {
        return byte_offset;
    }
    uint64 GetByteSize() const {
        return num_elements * stride;
    }
    class Buffer* GetBuffer() const {
        return buffer;
    }

    class Buffer* buffer;
    uint64        byte_offset;
    uint64        num_elements;
    uint32        stride;
    EPixelFormat  format = PF_UNDEFINED;
};

struct BufferInfo {
    uint64_t          size;
    uint32_t          stride;
    EBufferUsageFlags usage;
    EPixelFormat      format = PF_UNDEFINED;

    BufferInfo() = default;

    BufferInfo(uint64_t _size, uint32_t _stride, EBufferUsageFlags _usage, EPixelFormat _fmt = PF_UNDEFINED) :
        size(_size),
        stride(_stride),
        usage(_usage),
        format(_fmt) {}

    static BufferInfo GetNull() {
        return {0, 0, EBufferUsageFlags::NONE};
    }

    bool IsNull() const {
        return usage == EBufferUsageFlags::NONE && size == stride && size == 0;
    }
};

class Buffer : public RHIResource {
public:
    /**
	 * @brief Construct a new RHIBuffer object
	 *
	 * @param _info
	 */
    Buffer(const BufferInfo& _info) : RHIResource(RRT_BUFFER), info(_info) {}

    uint GetNumElement() const {
        return info.size;
    }
    uint64 GetByteSize() const {
        return info.size * info.stride;
    }
    uint GetStride() const {
        return info.stride;
    }
    EBufferUsageFlags GetUsage() const {
        return info.usage;
    }
    EPixelFormat GetFormat() const {
        return info.format;
    }
    const std::string_view GetName() const {
        return std::string_view(debug_name.has_value() ? debug_name.value().data() : default_name.data());
    }

    RENDER_API BufferView GetView(uint64 _byte_offset = 0, uint64 _byte_size = UINT64_MAX);
    RENDER_API BufferView GetView(EPixelFormat _fmt, uint64 _byte_offset = 0, uint64 _byte_size = UINT64_MAX);
    virtual RENDER_API void SetName(const std::string_view _name) = 0;
    // Owning readback requires both payload materialization and a backend
    // completion path capable of publishing Ready. Backends must opt in
    // explicitly so CommandList never returns a Future that is guaranteed to
    // fail after recording.
    [[nodiscard]] virtual bool SupportsOwningReadback() const noexcept {
        return false;
    }

protected:
    /**
	 * @brief Create an empty RHIBuffer, do nothing in rhi backend
	 *
	 */
    Buffer() : RHIResource(RRT_BUFFER) {}
    std::optional<std::string> debug_name;

protected:
    BufferInfo info;
};

struct RENDER_API TextureView {
public:
    TextureView() = default;
    TextureView(class Texture*);
    TextureView(TextureRef);
    TextureView(Texture* _texture, EPixelFormat _fmt, uint8 _mip_idx, uint8 _mip_cnt);
    class Texture* texture;
    EPixelFormat   format;
    uint3          offset{};
    uint3          extent{};
    uint8          mip_level = 0;
    uint8          num_mips;
    uint8          array_layer = 0;
    uint8          num_array;
    Texture*       GetTexture() const {
        return texture;
    }
    TextureView Slice(uint layer, uint count = 1) const;
};

template<typename TTexture>
struct TextureSubresourceKeyT {
    TTexture* texture{nullptr};
    uint8     mip_level{0};
    uint8     mip_count{1};
    uint8     array_layer{0};
    uint8     array_count{1};

    bool operator==(const TextureSubresourceKeyT& _other) const {
        return texture == _other.texture && mip_level == _other.mip_level && mip_count == _other.mip_count &&
               array_layer == _other.array_layer && array_count == _other.array_count;
    }
};

template<typename TTexture>
struct TextureSubresourceKeyHashT {
    size_t operator()(const TextureSubresourceKeyT<TTexture>& _key) const {
        size_t hash = std::hash<uint64>()(reinterpret_cast<uint64>(_key.texture));
        hash ^= std::hash<uint32>()(
                    (uint32(_key.mip_level) << 24) | (uint32(_key.mip_count) << 16) |
                    (uint32(_key.array_layer) << 8) | uint32(_key.array_count)
                ) +
                0x9e3779b9 + (hash << 6) + (hash >> 2);
        return hash;
    }
};

using TextureSubresourceKey     = TextureSubresourceKeyT<Texture>;
using TextureSubresourceKeyHash = TextureSubresourceKeyHashT<Texture>;

inline constexpr uint8 kRemainingSubresource = 0xFF;

inline void ValidateSubresourceRange(
    Texture* _texture,
    uint8    _mip_level,
    uint8    _mip_count,
    uint8    _array_layer,
    uint8    _array_count
);

struct TextureInfo {
    TextureInfo() = default;

    TextureInfo(ETextureDimension _dimension) : dimension(_dimension) {}

    TextureInfo(
        ETextureDimension  _dimension,
        ETextureUsageFlags _usage,
        EPixelFormat       _format,
        EClearAttachment   _clear_attachment,
        Extent3D           _extent,
        uint8_t            _num_mips    = 1u,
        uint8              _array_size  = 1u,
        uint8_t            _num_samples = 1u
    ) :
        dimension(_dimension),
        usage(_usage),
        format(_format),
        clear_attachment(_clear_attachment),
        extent(_extent.x, _extent.y),
        depth(_extent.z == 0 ? 1 : _extent.z),
        num_mips(_num_mips),
        array_size(_array_size),
        num_samples(_num_samples) {}

    ETextureUsageFlags usage = ETextureUsageFlags::UNDEFINED;

    Moer::Vector2i extent = Moer::Vector2i(1, 1);

    /** Depth of the texture if the dimension is 3D. */
    uint16_t depth = 1;

    /** The number of array elements in the texture. (Keep at 1 if dimension is 3D). */
    uint16_t array_size = 1;

    /** Number of mips in the texture mip-map chain. */
    uint8_t num_mips = 1;

    /** Number of samples in the texture. >1 for MSAA. */
    uint8_t num_samples = 1;

    /** Texture dimension to use when creating the RHI texture. */
    ETextureDimension dimension = ETextureDimension::TEX_2D;

    /** Pixel format used to create RHI texture. */
    EPixelFormat format = PF_UNDEFINED;

    /** Texture format used when creating the UAV. PF_Unknown means to use the default one (same as Format). */
    EPixelFormat uav_format = PF_UNDEFINED;

    RHIClearAttachment clear_attachment;

    ETextureAspectFlags aspect_flags = ETextureAspectFlags::COLOR;

    std::optional<std::string> debug_name;

    bool operator==(const TextureInfo& _other) const {
        return dimension == _other.dimension && usage == _other.usage && format == _other.format &&
               uav_format == _other.uav_format && extent == _other.extent && depth == _other.depth &&
               array_size == _other.array_size && num_mips == _other.num_mips &&
               num_samples == _other.num_samples && clear_attachment == _other.clear_attachment;
    }

    bool operator!=(const TextureInfo& _other) const {
        return !(*this == _other);
    }

    TextureInfo& operator=(const TextureInfo& _other) = default;
};

class Texture : public RHIResource {
public:
    Texture(const TextureInfo& _info) : RHIResource(RRT_TEXTURE), info(_info) {}

    uint32_t GetNumMips() const {
        return info.num_mips;
    }
    uint32_t GetNumArray() const {
        return info.array_size;
    }
    uint32_t GetNumSamples() const {
        return info.num_samples;
    }
    uint32_t GetDepth() const {
        return info.depth;
    }
    uint32_t GetWidth() const {
        return info.extent.x;
    }
    uint32_t GetHeight() const {
        return info.extent.y;
    }
    EPixelFormat GetFormat() const {
        return info.format;
    }
    ETextureDimension GetDimension() const {
        return info.dimension;
    }
    ETextureUsageFlags GetUsage() const {
        return info.usage;
    }
    ETextureAspectFlags GetAspectFlags() const {
        return info.aspect_flags;
    }
    uint3 GetExtent() const {
        return uint3(info.extent.x, info.extent.y, info.depth);
    }
    virtual uint           GetMipByteSize(uint _mip_idx) const = 0;
    const std::string_view GetName() const {
        return std::string_view(debug_name.has_value() ? debug_name.value().data() : default_name.data());
    }
    RENDER_API TextureView  GetView(uint8 _mip_idx = 0u, uint8 _mip_num = 1u);
    RENDER_API TextureView  GetView(EPixelFormat _format, uint8 _mip_idx = 0u, uint8 _mip_num = 1u);
    virtual RENDER_API void SetName(const std::string_view _name) = 0;
    [[nodiscard]] virtual bool SupportsOwningReadback() const noexcept {
        return false;
    }

protected:
    std::optional<std::string> debug_name;

private:
    friend DepthBuffer;
    TextureInfo info;
};

inline void ValidateSubresourceRange(
    Texture* _texture,
    uint8    _mip_level,
    uint8    _mip_count,
    uint8    _array_layer,
    uint8    _array_count
) {
    assert(_texture && "ValidateSubresourceRange requires a valid texture");
    uint8 max_mips   = _texture->GetNumMips();
    uint8 max_arrays = _texture->GetNumArray();
    uint8 mip_count  = _mip_count == kRemainingSubresource ? uint8(max_mips - _mip_level) : _mip_count;
    uint8 arr_count = _array_count == kRemainingSubresource ? uint8(max_arrays - _array_layer) : _array_count;

    assert(_mip_level < max_mips && "mip_level out of range");
    assert(mip_count >= 1 && "mip_count must be >= 1");
    assert(_mip_level + mip_count <= max_mips && "mip range out of bounds");

    assert(_array_layer < max_arrays && "array_layer out of range");
    assert(arr_count >= 1 && "array_count must be >= 1");
    assert(_array_layer + arr_count <= max_arrays && "array range out of bounds");
}

class DepthBuffer : public RHIResource {
    friend class RenderDevice;
    DepthBuffer(TextureRef _tex) : RHIResource(RRT_DEPTH), tex_handle(_tex) {}

public:
    uint GetNumMips() const {
        return tex_handle->GetNumMips();
    }
    uint GetNumArray() const {
        return tex_handle->GetNumArray();
    }
    uint GetDepth() const {
        return tex_handle->GetDepth();
    }
    uint GetWidth() const {
        return tex_handle->GetWidth();
    }
    uint GetHeight() const {
        return tex_handle->GetHeight();
    }
    EPixelFormat GetFormat() const {
        return tex_handle->GetFormat();
    }
    ETextureDimension GetDimension() const {
        return tex_handle->GetDimension();
    }
    ETextureUsageFlags GetUsage() const {
        return tex_handle->GetUsage();
    }
    ETextureAspectFlags GetAspectFlags() const {
        return tex_handle->GetAspectFlags();
    }
    uint3 GetExtent() const {
        return tex_handle->GetExtent();
    }
    const std::string_view GetName() const {
        return tex_handle->GetName();
    }
    RENDER_API TextureView GetView() {
        return tex_handle->GetView();
    }
    RENDER_API void SetName(const std::string_view _name) {
        tex_handle->SetName(_name);
    }

    TextureRef CastToTextureRef() {
        return tex_handle;
    }

private:
    TextureRef tex_handle;
};

struct VertexBuffer {
    Buffer* buffer;
    uint64  offset{0};
};
struct IndexBuffer {
    BufferView        buffer;
    EIndexElementType stride;
};

struct ImportTexture {
    TextureView   texture;
    ETextureState state;
};

struct ExportTexture {
    TextureView   texture;
    ETextureState state;
};

struct ImportBuffer {
    BufferView   buffer;
    EBufferState state;
};

struct ExportBuffer {
    BufferView   buffer;
    EBufferState state;
};

class RENDER_API BindlessArray : public RHIResource {
public:
    struct TextureUpdateInfo {
        TextureRef   texture;
        Sampler      sampler;
        EPixelFormat format;
        uint         array_idx;
        uint         slot;
        uint8        mip_level;
        uint8        num_mips;
        uint8        array_layer;
        uint8        array_count;
        uint64        array_generation;
        uint64        slot_generation;
        uint64        command_token;
        bool         free;
    };

    struct BufferUpdateInfo {
        BufferRef    buffer;
        uint         array_idx;
        uint         slot;
        EPixelFormat format;
        uint64       array_generation;
        uint64       slot_generation;
        uint64       command_token;
        bool         free;
    };

    struct InvalidUpdateInfo {
        uint array_idx;
    };

    using UpdateCmd = std::variant<TextureUpdateInfo, BufferUpdateInfo, InvalidUpdateInfo>;

    BindlessArray();
    virtual ~BindlessArray() = default;

    virtual uint AllocateTexture(const TextureView& _texture, Sampler _sampler) = 0;
    virtual uint AllocateBuffer(BufferView _buffer)                             = 0;

    virtual void UnbindTexture(uint _handle) = 0;
    virtual void UnbindBuffer(uint _handle)  = 0;

    virtual uint64 ArrayHandle() const = 0;

protected:
    friend class CommandList;
    friend class UpdateBindlessArrayCmd;

    virtual UniquePtr<class Command> CreateUpdateCommand() = 0;
    virtual void DiscardUpdateCommand(const Array<UpdateCmd>&) {}
};

struct ArrayArgReference {
    uint handle;

    uint operator()() const {
        return handle;
    }

    bool operator==(const ArrayArgReference& _other) const {
        return handle == _other.handle;
    }
    bool operator!=(const ArrayArgReference& _other) const {
        return handle != _other.handle;
    }
    bool operator<(const ArrayArgReference& _other) const {
        return handle < _other.handle;
    }
    bool operator>(const ArrayArgReference& _other) const {
        return handle > _other.handle;
    }
    bool operator<=(const ArrayArgReference& _other) const {
        return handle <= _other.handle;
    }
    bool operator>=(const ArrayArgReference& _other) const {
        return handle >= _other.handle;
    }

    ArrayArgReference() = default;
    ArrayArgReference(uint _handle) : handle(_handle) {}
    ArrayArgReference(const ArrayArgReference& _other) : handle(_other.handle) {}
    ArrayArgReference(ArrayArgReference&& _other) : handle(_other.handle) {
        _other.handle = 0;
    }
    ArrayArgReference& operator=(const ArrayArgReference& _other) {
        handle = _other.handle;
        return *this;
    }
    ArrayArgReference& operator=(ArrayArgReference&& _other) {
        handle        = _other.handle;
        _other.handle = 0;
        return *this;
    }
    ArrayArgReference& operator=(uint _handle) {
        handle = _handle;
        return *this;
    }
    operator uint() const {
        return handle;
    }
    operator bool() const {
        return handle != 0;
    }
};

/// <summary>
/// HWRayTracing
/// </summary>

// geometry segment in raytracing geometry

struct RaytracingSizeInfos {
    uint64 build_scratch_size;
    uint64 update_scratch_size;
    uint64 result_size;
};
struct RaytracingSegment {
    uint vertex_offset;
    uint index_offset;

    uint first_vertex;
    uint vertex_count;
    uint vertex_stride;
    uint first_primitive;
    uint primitive_count;

    BufferRef vertex_buffer = nullptr;
    BufferRef index_buffer  = nullptr;

    ERayTracingGeometryType  type             = RTGT_TRIANGLES;
    ERayTracingGeometryFlags flags            = ERayTracingGeometryFlags::NONE;
    bool                     b_force_opaque   = false;
    bool                     b_cull_back_face = false;
    bool                     b_flip_face      = false;
};
struct RaytracingGeometryInfo {
    Array<RaytracingSegment> segments = {};

    EIndexElementType index_type    = EIndexElementType::IET_UINT32;
    EPixelFormat      vertex_format = PF_R32G32B32_SFLOAT;

    ERayTracingAccelerationStructureBuildFlags build_flags = ERayTracingAccelerationStructureBuildFlags::NONE;
};
class RaytracingGeometry : public RHIResource {
public:
    RaytracingGeometry(const RaytracingGeometryInfo& _info) :
        info(_info),
        RHIResource(RRT_RAYTRACING_GEOMETRY) {}

    RaytracingGeometryInfo GetInfo() const {
        return info;
    }
    virtual Buffer* GetUnderlyingBuffer() const {
        return nullptr;
    }

protected:
    RaytracingGeometryInfo info;
};

struct AccelerationStructureBuildParam {
    RaytracingGeometryRef geometry;
    ERaytracingBuildMode  mode;
};

struct RaytracingMaterial {
    uint64 handle;
    uint64 sbt_offset;
};

struct RaytracingInstance {
    struct Flag {
        uint need_update : 1 = false;
        uint need_create : 1 = true;
    };

    RaytracingGeometry* geom; //src geom reference
    RaytracingMaterial  material_ref;
    Matrix3x4f          transform;

    const uint    array_idx;   //index in scene array
    const uint    instance_id; //raw data idx in soa representation
    uint          segment_idx  = 0;
    uint          custom_index = 0;
    RTVisibleMask visible_mask = RTVM_ALL;
    Flag          flag;
};

class RaytracingTlas : public RHIResource {
public:
    RaytracingTlas() : RHIResource(RRT_RAYTRACING_TLAS) {}
    virtual ~RaytracingTlas() = default;
    virtual Buffer* GetUnderlyingBuffer() const {
        return nullptr;
    }
};

//container for scene TLAS and rt instances
class RaytracingScene : public RHIResource {
public:
    RaytracingScene() : RHIResource(RRT_RAYTRACING_SCENE) {}
    virtual ~RaytracingScene() = default;

public:
    virtual RaytracingInstance& AddInstance()                 = 0;
    virtual void                FreeInstance(uint _array_idx) = 0;
    virtual void                MarkModified(uint _array_idx) = 0;
    virtual UniquePtr<Command>  UpdateScene()                 = 0;
    virtual void                AdvanceFrame()                = 0;

    virtual void RegisterGeometry(RaytracingGeometryRef _geom)   = 0;
    virtual void UnregisterGeometry(RaytracingGeometryRef _geom) = 0;

    virtual RaytracingTlasRef GetTlas() const     = 0;
    virtual RaytracingTlasRef GetPrevTlas() const = 0;

    RENDER_API RaytracingInstance&       GetInstance(uint _array_idx);
    RENDER_API const RaytracingInstance& GetInstance(uint _array_idx) const;
    RENDER_API uint                      GetInstanceCount() const;

protected:
    Array<RaytracingInstance> instances;
};
class Fence : public RHIResource {
public:
    Fence() : RHIResource(RRT_GPU_FENCE) {}
    virtual uint64_t GetValue() const      = 0;
    virtual void     Wait(uint64_t _value) = 0;

    // Native-submission acceptance is intentionally distinct from GPU
    // completion. A renderer history transaction may advance as soon as its
    // signal-owning submit has reached the native queue, but must not advance
    // merely because the frontend handoff accepted the CmdSubmit.
    virtual void MarkSubmitted(uint64_t _value) = 0;
    [[nodiscard]] virtual bool WaitSubmitted(
        uint64_t               _value,
        const std::atomic_bool* _continue_waiting = nullptr,
        EQueueType              _waiting_queue = EQueueType::Ignore,
        uint32                  _dependency_count = 0
    ) = 0;
    [[nodiscard]] virtual bool IsRejected(uint64_t _value) const = 0;

    // Terminalize a timeline value which can no longer be published because
    // its owning submission was rejected before reaching a GPU queue. This is
    // deliberately distinct from host-signalling the value: treating rejected
    // producer work as successfully complete could let dependent GPU work read
    // resources which were never written.
    virtual void Reject(uint64_t _value) noexcept = 0;
};

struct BackBufferInfo {
    TextureRef texture;
    FenceRef   fence;
    bool       Valid() const {
        return texture != nullptr;
    }
};

class Viewport : public RHIResource {
public:
    Viewport() : RHIResource(RRT_VIEWPORT) {}
    virtual ~Viewport() {}
    virtual void* GetNativeWindow()                = 0;
    virtual void  Present(FenceRef _present_fence) = 0;
    virtual void  Resize(Extent2D)                 = 0;
    // virtual BackBufferInfo GetBackBuffer()                  = 0;
};
} // namespace Moer::Render

#pragma region graphic pipeline definitions

struct RHIColorAttachmentInfo {
    RHIBlendAttachmentInfo blend_state_info;
    RHIClearAttachment     clear_attachment;
    EPixelFormat           pixel_format;
    ETextureUsageFlags     usage_flags;

    template<
        Moer::Render::Blend     blend_mode = Moer::Render::Blend::NONE,
        Moer::Render::ClearMode clear_mode = Moer::Render::ClearMode::COLOR>
    static RHIColorAttachmentInfo Preset(
        EPixelFormat       _pixel_format,
        ETextureUsageFlags _usage_flags = ETextureUsageFlags::COLOR_ATTACHMENT
    ) {
        RHIColorAttachmentInfo info;
        info.blend_state_info = std::move(RHIBlendAttachmentInfo::Preset<blend_mode>());
        info.clear_attachment = std::move(RHIClearAttachment::Preset<clear_mode>());
        info.pixel_format     = _pixel_format;
        info.usage_flags      = ETextureUsageFlags::COLOR_ATTACHMENT;

        return std::move(info);
    }

    RHIColorAttachmentInfo& SetBlendStateInfo(RHIBlendAttachmentInfo&& _blend_state_info) {
        blend_state_info = _blend_state_info;
        return *this;
    }

    bool operator==(const RHIColorAttachmentInfo& other) const {
        return blend_state_info == other.blend_state_info && clear_attachment == other.clear_attachment &&
               pixel_format == other.pixel_format && usage_flags == other.usage_flags;
    }
};

namespace Moer::Render {
struct ParamInfoFlags {
    uint64 state_flags;
    uint64 pipeline_flags;
};

class PipelineState : public RHIResource {
public:
    PipelineState() : RHIResource(RRT_PIPELINE_STATE) {}
};
struct PipelineHandle {
    uint64                     handle = 0;
    Array<ParamInfoFlags>      binding_infos;
    UnorderedMap<uint64, uint> hash_2_info_index; // not use
    uint64                     valid_bits   = 0;  // the pipeline actually used resource
    int                        constant_idx = -1; // not use

    bool IsValid() const {
        return handle != 0;
    }
};

struct SingleShaderInfo {
    std::string_view         name;
    std::string_view         entry_point;
    std::span<uint8>         shader_data;
    EShaderType              shader_type;
    ShaderParametersInfoMap* shader_param_map = nullptr;
};

struct ShaderVsGsPs {
    SingleShaderInfo vs;
    SingleShaderInfo gs;
    SingleShaderInfo ps;
};

struct ShaderCs {
    SingleShaderInfo cs;
};

struct ShaderVsPs {
    SingleShaderInfo vs;
    SingleShaderInfo ps;
};

struct ShaderVsHsDsPs {
    SingleShaderInfo vs;
    SingleShaderInfo hs;
    SingleShaderInfo ds;
    SingleShaderInfo ps;
};

struct ShaderMsPs {
    SingleShaderInfo ms;
    SingleShaderInfo ps;
};

struct ShaderTsMsPs {
    SingleShaderInfo ts;
    SingleShaderInfo ms;
    SingleShaderInfo ps;
};

struct ShaderRT {
    Array<SingleShaderInfo> raygen;
    Array<SingleShaderInfo> miss;
    Array<SingleShaderInfo> hit;
    Array<SingleShaderInfo> closesthit;
    Array<SingleShaderInfo> callable;
};

enum EShaderArgType : uint8 {
    SDA_Buffer,
    SDA_ConstantBuffer,
    SDA_Texture,
    SDA_Sampler,
    SDA_Constant,
    SDA_BindlessArray,
    SDA_TLAS,
    SDA_Reference,
    SDA_Num
};

struct ShaderArgCppInfo {
    uint           array_size;
    EShaderArgType type;
};

using ShaderOutputGroup =
    std::variant<ShaderVsGsPs, ShaderVsPs, ShaderVsHsDsPs, ShaderMsPs, ShaderTsMsPs, ShaderCs, ShaderRT>;

struct PipelineShaderInfo {
    ShaderOutputGroup       shader_group;
    Array<std::string_view> layout_hash;
    Array<ShaderArgCppInfo> arg_cpp_info;
};

struct GfxPsoCreateInfo {
    using RHIColorAttachmentInfoList = Moer::StaticArray<RHIColorAttachmentInfo, MAX_PASS_ATTACHMENT_COUNT>;

    GfxPsoCreateInfo(
        RHIRasterizeInfo              _rasterizer_info,
        VertexStream                  _vertex_stream,
        Array<RHIColorAttachmentInfo> _color_attachments_info,
        RHIDepthStencilStateInfo      _depth_stencil_info,
        EPixelFormat                  _depth_stencil_format,
        EPrimitiveTopology            _primitive_topology                 = EPrimitiveTopology::TRIANGLE_LIST,
        RHIMultisampleStateInfo       _multisample_info                   = RHIMultisampleStateInfo::Preset(),
        uint8_t                       _multi_view_count                   = 1,
        bool                          _b_has_fragment_density_attachments = false,
        EVariousShadingRate           _shading_rate                       = EVariousShadingRate::VSR_1_1x1
    ) :
        rasterizer_info(std::move(_rasterizer_info)),
        vertex_stream(std::move(_vertex_stream)),
        multisample_info(std::move(_multisample_info)),
        depth_stencil_info(std::move(_depth_stencil_info)),
        primitive_topology(_primitive_topology),
        color_attachments_info(_color_attachments_info),
        color_attachment_count(_color_attachments_info.size()),
        depth_stencil_format(_depth_stencil_format),
        multi_view_count(_multi_view_count),
        b_has_fragment_density_attachments(_b_has_fragment_density_attachments),
        shading_rate(_shading_rate),
        hash_key(0) {}

    GfxPsoCreateInfo(
        RHIRasterizeInfo              _rasterizer_info,
        VertexStream                  _vertex_stream,
        Array<RHIColorAttachmentInfo> _color_attachments_info
    ) :
        rasterizer_info(std::move(_rasterizer_info)),
        vertex_stream(std::move(_vertex_stream)),
        multisample_info(RHIMultisampleStateInfo::Preset()),
        depth_stencil_info(RHIDepthStencilStateInfo::Preset()),
        primitive_topology(EPrimitiveTopology::TRIANGLE_LIST),
        color_attachments_info(_color_attachments_info),
        color_attachment_count(_color_attachments_info.size()),
        depth_stencil_format(PF_UNDEFINED),
        multi_view_count(1),
        b_has_fragment_density_attachments(false),
        shading_rate(VSR_1_1x1),
        hash_key(0) {}

    GfxPsoCreateInfo& SetPatchControlPoints(uint32_t control_points) {
        patch_control_points = control_points;
        return *this;
    }

    RHIRasterizeInfo         rasterizer_info;
    VertexStream             vertex_stream;
    RHIMultisampleStateInfo  multisample_info;
    RHIDepthStencilStateInfo depth_stencil_info;

    EPrimitiveTopology primitive_topology;
    // Required when primitive_topology is PATCH_LIST. Kept explicit instead of assuming triangle
    // patches so the RHI contract remains valid for future isoline/quad tessellation passes.
    uint32_t patch_control_points = 0;

    EPixelFormat                  depth_stencil_format;
    Array<RHIColorAttachmentInfo> color_attachments_info;

    uint32_t color_attachment_count;

    uint8_t multi_view_count = 1;

    //for VSR
    bool                b_has_fragment_density_attachments;
    EVariousShadingRate shading_rate;

    uint64_t hash_key;
};
}; // namespace Moer::Render

enum EAttachmentAction : uint8_t {
    /* for inner definition use, do not use directly */
    INNER_DEPTH_MASK_OFFSET = 4,
#define MAKE_COLOR_ACTION_MASK(LOAD, STORE)                                      \
    ((uint8_t)EAttachmentLoadOp::LOAD << (uint32_t)EAttachmentStoreOp::NumBits | \
     (uint8_t)EAttachmentStoreOp::STORE)
    AC_NO_LOAD_NO_STORE = MAKE_COLOR_ACTION_MASK(NONE, NONE),
    AC_LOAD_NO_STORE    = MAKE_COLOR_ACTION_MASK(LOAD, NONE),
    AC_LOAD_STORE       = MAKE_COLOR_ACTION_MASK(LOAD, STORE),
    AC_CLEAR_NO_STORE   = MAKE_COLOR_ACTION_MASK(CLEAR, NONE),
    AC_NO_LOAD_STORE    = MAKE_COLOR_ACTION_MASK(NONE, STORE),
    AC_CLEAR_STORE      = MAKE_COLOR_ACTION_MASK(CLEAR, STORE),
    AC_CLEAR_RESOLVE    = MAKE_COLOR_ACTION_MASK(CLEAR, MULTISAMPLE_RESOLVE),
    AC_LOAD_RESOLVE     = MAKE_COLOR_ACTION_MASK(CLEAR, MULTISAMPLE_RESOLVE),
#undef MAKE_COLOR_ACTION_MASK

#define MAKE_DEPTH_STENCIL_MASK(DEPTH, STENCIL)                               \
    ((uint8_t)EAttachmentAction::DEPTH << (uint32_t)INNER_DEPTH_MASK_OFFSET | \
     (uint8_t)EAttachmentAction::STENCIL)
    AC_DS_NO_LOAD_NO_STORE   = MAKE_DEPTH_STENCIL_MASK(AC_NO_LOAD_NO_STORE, AC_NO_LOAD_NO_STORE),
    AC_DS_STORE              = MAKE_DEPTH_STENCIL_MASK(AC_NO_LOAD_STORE, AC_NO_LOAD_STORE),
    AC_DS_DEPTH_STORE        = MAKE_DEPTH_STENCIL_MASK(AC_NO_LOAD_STORE, AC_NO_LOAD_NO_STORE),
    AC_DS_STENCIL_STORE      = MAKE_DEPTH_STENCIL_MASK(AC_NO_LOAD_NO_STORE, AC_NO_LOAD_STORE),
    AC_DS_CLEAR_STORE        = MAKE_DEPTH_STENCIL_MASK(AC_CLEAR_STORE, AC_CLEAR_STORE),
    AC_DS_LOAD_STORE         = MAKE_DEPTH_STENCIL_MASK(AC_LOAD_STORE, AC_LOAD_STORE),
    AC_DS_LOAD_STORE_DEPTH   = MAKE_DEPTH_STENCIL_MASK(AC_LOAD_STORE, AC_NO_LOAD_NO_STORE),
    AC_DS_LOAD_DEPTH         = MAKE_DEPTH_STENCIL_MASK(AC_LOAD_NO_STORE, AC_NO_LOAD_NO_STORE),
    AC_DS_LOAD_STORE_STENCIL = MAKE_DEPTH_STENCIL_MASK(AC_LOAD_NO_STORE, AC_LOAD_STORE),

    AC_DS_CLEAR_NO_STORE                 = MAKE_DEPTH_STENCIL_MASK(AC_CLEAR_NO_STORE, AC_CLEAR_NO_STORE),
    AC_DS_LOAD_NO_STORE                  = MAKE_DEPTH_STENCIL_MASK(AC_LOAD_NO_STORE, AC_LOAD_NO_STORE),
    AC_DS_CLEAR_STORE_DEPTH              = MAKE_DEPTH_STENCIL_MASK(AC_CLEAR_STORE, AC_CLEAR_NO_STORE),
    AC_DS_CLEAR_STORE_STENCIL            = MAKE_DEPTH_STENCIL_MASK(AC_CLEAR_NO_STORE, AC_CLEAR_STORE),
    AC_DS_CLEAR_RESOLVE_DEPTH            = MAKE_DEPTH_STENCIL_MASK(AC_CLEAR_RESOLVE, AC_CLEAR_NO_STORE),
    AC_DS_CLEAR_RESOLVE_STENCIL          = MAKE_DEPTH_STENCIL_MASK(AC_CLEAR_NO_STORE, AC_CLEAR_RESOLVE),
    AC_DS_LOAD_DEPTH_CLEAR_STENCIL_STORE = MAKE_DEPTH_STENCIL_MASK(AC_LOAD_STORE, AC_CLEAR_STORE),
    AC_DS_CLEAR_STENCIL_STORE_STENCIL    = MAKE_DEPTH_STENCIL_MASK(AC_NO_LOAD_NO_STORE, AC_CLEAR_STORE)
#undef MAKE_DEPTH_STENCIL_MASK
};

FORCEINLINE EAttachmentAction GetDepthAction(EAttachmentAction _depth_stencil_action) {
    return (EAttachmentAction)(_depth_stencil_action >> (uint32_t)INNER_DEPTH_MASK_OFFSET);
}
FORCEINLINE EAttachmentAction GetStencilAction(EAttachmentAction _depth_stencil_action) {
    return (EAttachmentAction)(_depth_stencil_action & ((1 << (uint32_t)INNER_DEPTH_MASK_OFFSET) - 1));
}
FORCEINLINE EAttachmentLoadOp GetLoadOp(EAttachmentAction _load_store_action) {
    return (EAttachmentLoadOp)(_load_store_action >> (uint32_t)EAttachmentStoreOp::NumBits);
}
FORCEINLINE EAttachmentStoreOp GetStoreOp(EAttachmentAction _load_store_action) {
    return (EAttachmentStoreOp)(_load_store_action & ((1 << (uint32_t)EAttachmentStoreOp::NumBits)) - 1);
}

namespace Moer::Render {
struct ColorAttachment {
    Texture*          target;
    EAttachmentAction action      = AC_CLEAR_STORE;
    float4            clear_color = {0, 0, 0, 0};
    uint              mip_level   = 0;
    uint              array_layer = 0;
};

// 聚合初始化会触发零初始化，但构造函数不会，所以必须设置默认值
struct DepthAttachment {
    Texture*          target{nullptr};
    uint              array_layer   = 0;
    uint              mip_level     = 0;
    EAttachmentAction action        = AC_DS_CLEAR_STORE;
    float             clear_depth   = 0.f;
    uint              clear_stencil = 0;
    bool              Valid() const {
        return target != nullptr;
    }
    DepthAttachment() = default;
    DepthAttachment(Texture* _tex) : target(_tex) {}
    DepthAttachment(const TextureView& _view) :
        target(_view.GetTexture()),
        array_layer(_view.array_layer),
        mip_level(_view.mip_level) {}
};

struct RenderPassInfo {
    Array<ColorAttachment> color_attachments;
    DepthAttachment        depth_attachment;
    Rect2D                 render_area;
    uint                   viewport_cnt = 1;
};

struct SwapchainCreateInfo {
    uintptr_t    window_handle;
    Extent2D     size;
    uint         back_buffer_sz   = 2;
    EPixelFormat preferred_format = PF_R8G8B8A8_SRGB;
};

class RENDER_API Swapchain : public RHIResource {
protected:
    Swapchain() : RHIResource(RRT_SWAPCHAIN) {};

public:
    virtual void Recreate(const SwapchainCreateInfo&) = 0;
    virtual ~Swapchain()                              = default;
    virtual void Sync()                               = 0;

public:
    EPixelFormat format;
    Extent2D     size;
};
} // namespace Moer::Render

#pragma endregion

#endif // !RHI_RESOURCE_H
