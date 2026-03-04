#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "RenderGraphAllocator.h"
#include "rhi/RHIResource.h"

namespace Moer::Render::RenderGraph {

using TCHAR = char;

/** Render graph event name used for debugging and profiling. */
class FRDGEventName final {
public:
    FRDGEventName() = default;

    // Constructor for direct string (no formatting)
    explicit FRDGEventName(const char* InEventName) : EventName(InEventName) {}

    // Constructor for formatted string (printf-style)
    template<typename... Args>
    FRDGEventName(const char* InEventFormat, Args&&... args) {
        char Buffer[512];
        snprintf(Buffer, sizeof(Buffer), InEventFormat, std::forward<Args>(args)...);
        FormattedEventName = Buffer;
        EventName          = FormattedEventName.c_str();
    }

    FRDGEventName(const FRDGEventName&)            = default;
    FRDGEventName& operator=(const FRDGEventName&) = default;

    const char* GetCStr() const {
        return EventName;
    }

private:
    const char* EventName = "";
    std::string FormattedEventName; // Only used when formatting is needed
};

enum class ERDGHandleRegistryDestructPolicy {
    Registry,
    Allocator,
    Never
};

enum class ERDGViewableResourceType : uint8 {
    Texture,
    Buffer,
    NUM
};

/** The set of concrete view types. */
enum class ERDGViewType : uint8 {
    TextureUAV,
    TextureSRV,
    BufferUAV,
    BufferSRV,
    MAX
};

// inline constexpr ERDGViewableResourceType GetParentType(ERDGViewType ViewType) {
//     switch (ViewType) {
//         case ERDGViewType::TextureUAV:
//         case ERDGViewType::TextureSRV:
//             return ERDGViewableResourceType::Texture;
//         case ERDGViewType::BufferUAV:
//         case ERDGViewType::BufferSRV:
//             return ERDGViewableResourceType::Buffer;
//         default:
//             assert(!"Enclosing block should never be called");
//             return ERDGViewableResourceType::NUM;
//     }
// }

enum class ERDGUnorderedAccessViewFlags : uint8 {
    None        = 0,
    SkipBarrier = 1 << 0
};

ENUM_BIT_OP_IMPL(ERDGUnorderedAccessViewFlags, FLAG)

template<
    typename LocalHandleType,
    ERDGHandleRegistryDestructPolicy DestructPolicy = ERDGHandleRegistryDestructPolicy::Never>
class TRDGHandleRegistry {
public:
    using HandleType = LocalHandleType;
    using IndexType  = typename HandleType::IndexType;
    using ObjectType = typename HandleType::ObjectType;

    TRDGHandleRegistry()                                     = default;
    TRDGHandleRegistry(const TRDGHandleRegistry&)            = delete;
    TRDGHandleRegistry& operator=(const TRDGHandleRegistry&) = delete;
    ~TRDGHandleRegistry() {
        Clear();
    }

    HandleType Insert(ObjectType* Object) {
        Array.emplace_back(Object);
        Object->Handle = Last();
        return Last();
    }

    template<typename DerivedType = ObjectType, typename... TArgs>
    DerivedType* Allocate(FRDGAllocator& Allocator, TArgs&&... Args) {
        static_assert(
            std::is_base_of<ObjectType, DerivedType>::value, "DerivedType must inherit from ObjectType"
        );

        DerivedType* Object = nullptr;

        if constexpr (DestructPolicy == ERDGHandleRegistryDestructPolicy::Registry) {
            Object = Allocator.Alloc<DerivedType>(std::forward<TArgs>(Args)...);
        } else {
            Object = Allocator.AllocNoDestruct<DerivedType>(std::forward<TArgs>(Args)...);
        }

        Insert(Object);
        return Object;
    }

    void Clear() {
        if (DestructPolicy == ERDGHandleRegistryDestructPolicy::Registry) {
            for (int32_t i = (int32_t)Array.size() - 1; i >= 0; --i) {
                if (Array[i]) {
                    Array[i]->~ObjectType();
                }
            }
        }
        Array.clear();
    }

    inline ObjectType* operator[](HandleType Handle) {
        return Array[Handle.GetIndex()];
    }

    inline const ObjectType* operator[](HandleType Handle) const {
        return Array[Handle.GetIndex()];
    }

    template<typename FunctionType>
    void Enumerate(FunctionType Function) {
        for (ObjectType* Object : Array) {
            Function(Object);
        }
    }

    template<typename FunctionType>
    void Enumerate(FunctionType Function) const {
        for (const ObjectType* Object : Array) {
            Function(Object);
        }
    }

    inline const ObjectType* Get(HandleType Handle) const {
        return Array[Handle.GetIndex()];
    }

    inline ObjectType* Get(HandleType Handle) {
        return Array[Handle.GetIndex()];
    }

    inline HandleType Begin() const {
        return HandleType(0);
    }

    inline HandleType End() const {
        return HandleType(static_cast<IndexType>(Array.size()));
    }

    inline HandleType Last() const {
        return HandleType(static_cast<IndexType>(Array.size() - 1));
    }

    inline uint32_t Num() const {
        return (uint32_t)Array.size();
    }

private:
    Array<ObjectType*> Array;
};

struct FRDGBitReference {
    uint64_t& DataWord;
    uint64_t  BitMask;

    FRDGBitReference(uint64_t& InDataWord, uint64_t InBitMask) : DataWord(InDataWord), BitMask(InBitMask) {}

    void operator=(bool Value) {
        if (Value)
            DataWord |= BitMask;
        else
            DataWord &= ~BitMask;
    }

    operator bool() const {
        return (DataWord & BitMask) != 0;
    }
};

template<typename HandleType>
    requires requires(HandleType h) {
        { h.GetIndex() } -> std::convertible_to<uint32_t>;
    }
class TRDGHandleBitArray {
public:
    TRDGHandleBitArray() = default;

    void Init(uint32_t Count) {
        NumBits           = Count;
        uint32_t NumWords = (Count + 63) / 64;
        Data.assign(NumWords, 0);
    }

    FRDGBitReference operator[](HandleType Handle) {
        uint32_t Index = Handle.GetIndex();
        return FRDGBitReference(Data[Index / 64], 1ULL << (Index % 64));
    }

    bool operator[](HandleType Handle) const {
        uint32_t Index = Handle.GetIndex();
        return (Data[Index / 64] & (1ULL << (Index % 64))) != 0;
    }

    void Reset() {
        std::fill(Data.begin(), Data.end(), 0);
    }

    uint32_t Count() const {
        return NumBits;
    }

private:
    Array<uint64_t> Data;
    uint32_t        NumBits = 0;
};

template<typename LocalObjectType, typename LocalIndexType>
class TRDGHandle {
public:
    using ObjectType = LocalObjectType;
    using IndexType  = LocalIndexType;

    TRDGHandle() : Index(kNullIndex) {}
    explicit TRDGHandle(IndexType InIndex) : Index(InIndex) {}

    operator bool() const {
        return IsValid();
    }
    bool IsValid() const {
        return Index != kNullIndex;
    }
    IndexType GetIndex() const {
        if (!IsValid()) {
            throw std::runtime_error("Invalid RenderGraph handle access");
        }
        return Index;
    }
    IndexType GetIndexUnchecked() const {
        return Index;
    }

    bool operator==(TRDGHandle Other) const {
        return Index == Other.Index;
    }
    bool operator!=(TRDGHandle Other) const {
        return Index != Other.Index;
    }
    bool operator<(TRDGHandle Other) const {
        return Index < Other.Index;
    }
    bool operator<=(TRDGHandle Other) const {
        return Index <= Other.Index;
    }
    bool operator>(TRDGHandle Other) const {
        return Index > Other.Index;
    }
    bool operator>=(TRDGHandle Other) const {
        return Index >= Other.Index;
    }

    static TRDGHandle Min(TRDGHandle A, TRDGHandle B) {
        if (!A)
            return B;
        if (!B)
            return A;
        return A.Index < B.Index ? A : B;
    }

    static TRDGHandle Max(TRDGHandle A, TRDGHandle B) {
        return A.Index > B.Index ? A : B;
    }

private:
    static constexpr IndexType kNullIndex = std::numeric_limits<IndexType>::max();
    IndexType                  Index;
};

struct FRDGTextureDesc : public TextureInfo {
    static FRDGTextureDesc Create2D(
        Extent2D           Size,
        EPixelFormat       Format,
        EClearAttachment   ClearAttachment,
        ETextureUsageFlags Usage,
        uint8              NumMips    = 1,
        uint8              NumSamples = 1
    ) {
        const uint16 Depth     = 1;
        const uint16 ArraySize = 1;
        return FRDGTextureDesc(
            ETextureDimension::TEX_2D,
            Usage,
            Format,
            ClearAttachment,
            Extent3D(Size, Depth),
            Depth,
            ArraySize,
            NumMips,
            NumSamples
        );
    }

    static FRDGTextureDesc Create2DArray(
        Extent2D           Size,
        EPixelFormat       Format,
        EClearAttachment   ClearAttachment,
        ETextureUsageFlags Usage,
        uint16             ArraySize,
        uint8              NumMips    = 1,
        uint8              NumSamples = 1
    ) {
        const uint16 Depth = 1;
        return FRDGTextureDesc(
            ETextureDimension::TEX_2D_ARRAY,
            Usage,
            Format,
            ClearAttachment,
            Extent3D(Size, Depth),
            Depth,
            ArraySize,
            NumMips,
            NumSamples
        );
    }

    static FRDGTextureDesc Create3D(
        Extent3D           Size,
        EPixelFormat       Format,
        EClearAttachment   ClearAttachment,
        ETextureUsageFlags Usage,
        uint8              NumMips = 1
    ) {
        const uint16 ArraySize  = 1;
        const uint8  NumSamples = 1;
        const uint16 Depth      = static_cast<uint16>(Size.z == 0 ? 1 : Size.z);

        return FRDGTextureDesc(
            ETextureDimension::TEX_3D,
            Usage,
            Format,
            ClearAttachment,
            Size,
            Depth,
            ArraySize,
            NumMips,
            NumSamples
        );
    }

    static FRDGTextureDesc CreateCube(
        uint32             Size,
        EPixelFormat       Format,
        EClearAttachment   ClearAttachment,
        ETextureUsageFlags Usage,
        uint8              NumMips    = 1,
        uint8              NumSamples = 1
    ) {
        const uint16 Depth     = 1;
        const uint16 ArraySize = 6;
        return FRDGTextureDesc(
            ETextureDimension::TEX_CUBE,
            Usage,
            Format,
            ClearAttachment,
            Extent3D(Size, Size, Depth),
            Depth,
            ArraySize,
            NumMips,
            NumSamples
        );
    }

    static FRDGTextureDesc CreateCubeArray(
        uint32             Size,
        EPixelFormat       Format,
        EClearAttachment   ClearAttachment,
        ETextureUsageFlags Usage,
        uint16             ArraySize,
        uint8              NumMips    = 1,
        uint8              NumSamples = 1
    ) {
        const uint16 Depth = 1;
        return FRDGTextureDesc(
            ETextureDimension::TEX_CUBE_ARRAY,
            Usage,
            Format,
            ClearAttachment,
            Extent3D(Size, Size, Depth),
            Depth,
            static_cast<uint16>(ArraySize * 6u),
            NumMips,
            NumSamples
        );
    }

    static FRDGTextureDesc CreateRenderTargetTextureDesc(
        Extent2D           Size,
        EPixelFormat       Format,
        EClearAttachment   ClearAttachment,
        ETextureUsageFlags Usage,
        const bool         bRequireMultiView,
        uint16             MobileMultiViewRenderTargetNumLayers = 2
    ) {
        if (bRequireMultiView) {
            return FRDGTextureDesc::Create2DArray(
                Size, Format, ClearAttachment, Usage, MobileMultiViewRenderTargetNumLayers
            );
        }

        return FRDGTextureDesc::Create2D(Size, Format, ClearAttachment, Usage);
    }

    FRDGTextureDesc() = default;
    //TODO:使用FClearValueBinding
    FRDGTextureDesc(
        ETextureDimension  InDimension,
        ETextureUsageFlags InFlags,
        EPixelFormat       InFormat,
        EClearAttachment   InClearAttachment,
        Extent3D           InExtent,
        uint16             InDepth,
        uint16             InArraySize,
        uint8              InNumMips,
        uint8              InNumSamples //MSAA Samples
    ) :
        TextureInfo(
            InDimension,
            InFlags,
            InFormat,
            InClearAttachment,
            Extent3D(InExtent.x, InExtent.y, InDepth),
            InNumMips,
            InArraySize,
            InNumSamples
        ) {}
};

/** Flags to annotate a render graph texture. */
enum class ERDGTextureFlags : uint8 {
    None = 0,

    /** Tag the texture to survive through frame, that is important for multi GPU alternate frame rendering. */
    MultiFrame = 1 << 0,

    /** The buffer is ignored by RDG tracking and will never be transitioned. Use the flag when registering a buffer with no writable GPU flags.
	 *  Write access is not allowed for the duration of the graph. This flag is intended as an optimization to cull out tracking of read-only
	 *  buffers that are used frequently throughout the graph. Note that it's the user's responsibility to ensure the resource is in the correct
	 *  readable state for use with RDG passes, as RDG does not know the exact state of the resource.
	 */
    SkipTracking = 1 << 1,

    /** When set, RDG will perform its first barrier without splitting. Practically, this means the resource is left in its initial state
	 *  until the first pass it's used within the graph. Without this flag, the resource is split-transitioned at the start of the graph.
	 */
    ForceImmediateFirstBarrier = 1 << 2,

    /** Prevents metadata decompression on this texture. */
    MaintainCompression = 1 << 3,
};

ENUM_BIT_OP_IMPL(ERDGTextureFlags, FLAG)

/** Flags to annotate a render graph buffer. */
enum class ERDGBufferFlags : uint8 {
    None = 0,

    /** Tag the buffer to survive through frame, that is important for multi GPU alternate frame rendering. */
    MultiFrame = 1 << 0,

    /** The buffer is ignored by RDG tracking and will never be transitioned. Use the flag when registering a buffer with no writable GPU flags.
	 *  Write access is not allowed for the duration of the graph. This flag is intended as an optimization to cull out tracking of read-only
	 *  buffers that are used frequently throughout the graph. Note that it's the user's responsibility to ensure the resource is in the correct
	 *  readable state for use with RDG passes, as RDG does not know the exact state of the resource.
	 */
    SkipTracking = 1 << 1,

    /** When set, RDG will perform its first barrier without splitting. Practically, this means the resource is left in its initial state
	 *  until the first pass it's used within the graph. Without this flag, the resource is split-transitioned at the start of the graph.
	 */
    ForceImmediateFirstBarrier = 1 << 2
};

ENUM_BIT_OP_IMPL(ERDGBufferFlags, FLAG)

enum class ERDGPassFlags : uint16 {
    /** Pass doesn't have any inputs or outputs tracked by the graph. This may only be used by the parameterless AddPass function. */
    None = 0,

    /** Pass uses rasterization on the graphics pipe. */
    Raster = 1 << 0,

    /** Pass uses compute on the graphics pipe. */
    Compute = 1 << 1,

    /** Pass uses compute on the async compute pipe. */
    AsyncCompute = 1 << 2,

    /** Pass uses copy commands on the graphics pipe. */
    Copy = 1 << 3,

    /** Pass (and its producers) will never be culled. Necessary if outputs cannot be tracked by the graph. */
    NeverCull = 1 << 4,

    /** Render pass begin / end is skipped and left to the user. Only valid when combined with 'Raster'. Disables render pass merging for the pass. */
    SkipRenderPass = 1 << 5,

    /** Pass will never have its render pass merged with other passes. */
    NeverMerge = 1 << 6,

    /** Pass will never run off the render thread. */
    NeverParallel = 1 << 7,

    /** Pass uses copy commands but writes to a staging resource. */
    Readback = Copy | NeverCull
};

ENUM_BIT_OP_IMPL(ERDGPassFlags, FLAG)

// Type Definitions

class FRDGPass;
class FRDGView;
class FRDGTexture;
class FRDGBuffer;

using FRDGPassHandle   = TRDGHandle<FRDGPass, uint32>;
using FRDGPassBitArray = TRDGHandleBitArray<FRDGPassHandle>;
using FRDGPassRegistry = TRDGHandleRegistry<FRDGPassHandle>;

using RDGPassHandle   = FRDGPassHandle;
using RDGPassBitArray = FRDGPassBitArray;

using FRDGViewHandle   = TRDGHandle<FRDGView, uint32>;
using FRDGViewRegistry = TRDGHandleRegistry<FRDGViewHandle, ERDGHandleRegistryDestructPolicy::Never>;
using FRDGViewBitArray = TRDGHandleBitArray<FRDGViewHandle>;

using FRDGTextureHandle   = TRDGHandle<FRDGTexture, uint32>;
using FRDGTextureRegistry = TRDGHandleRegistry<FRDGTextureHandle, ERDGHandleRegistryDestructPolicy::Never>;
using FRDGTextureBitArray = TRDGHandleBitArray<FRDGTextureHandle>;

using FRDGBufferHandle               = TRDGHandle<FRDGBuffer, uint32>;
using FRDGBufferReservedCommitHandle = TRDGHandle<FRDGBuffer, uint16>;
using FRDGBufferRegistry = TRDGHandleRegistry<FRDGBufferHandle, ERDGHandleRegistryDestructPolicy::Never>;
using FRDGBufferBitArray = TRDGHandleBitArray<FRDGBufferHandle>;

using FRDGBufferNumElementsCallback     = std::function<uint32()>;
using FRDGBufferInitialDataCallback     = std::function<const void*()>;
using FRDGBufferInitialDataSizeCallback = std::function<uint64()>;

using FRDGPassHandlesByPipeline = TRHIPipelineArray<FRDGPassHandle>;
using FRDGPassesByPipeline      = TRHIPipelineArray<FRDGPass*>;

template<typename T>
struct IsStdVector : std::false_type {};
template<typename T, typename Allocator>
struct IsStdVector<std::vector<T, Allocator>> : std::true_type {};
template<typename T>
inline constexpr bool is_std_vector_v = IsStdVector<T>::value;

template<
    typename ArrayType,
    typename ArrayTypeNoRef = std::remove_reference_t<ArrayType>,
    typename                = std::enable_if_t<is_std_vector_v<ArrayTypeNoRef>>>
using TRDGBufferArrayCallback           = std::function<const ArrayType&()>;
using FRDGBufferInitialDataFreeCallback = std::function<void(const void* InData)>;
using FRDGBufferInitialDataFillCallback = std::function<void(void* InData, uint32 InDataSize)>;
using FRDGDispatchGroupCountCallback    = std::function<uint3()>;
} // namespace Moer::Render::RenderGraph
