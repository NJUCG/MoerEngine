#pragma once
#include <cstdint>
#include <functional>
#include <limits>

#include "RenderGraphAllocator.h"
#include "rhi/RHIResource.h"

namespace Moer {

using Render::TextureInfo;

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
        return HandleType(Array.Num());
    }

    inline HandleType Last() const {
        return HandleType(Array.Num() - 1);
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

#region Type Definitions

class FRDGPass;
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

using FRDGBufferNumElementsCallback     = std::function<uint32()>;
using FRDGBufferInitialDataCallback     = std::function<const void*()>;
using FRDGBufferInitialDataSizeCallback = std::function<uint64()>;
template<
    typename ArrayType,
    typename ArrayTypeNoRef = std::remove_reference_t<ArrayType>,
    typename                = typename TEnableIf<TIsTArray_V<ArrayTypeNoRef>>::Type>
using TRDGBufferArrayCallback           = std::function<const ArrayType&()>;
using FRDGBufferInitialDataFreeCallback = std::function<void(const void* InData)>;
using FRDGBufferInitialDataFillCallback = std::function<void(void* InData, uint32 InDataSize)>;
using FRDGDispatchGroupCountCallback    = std::function<FIntVector()>;
#pragma endregion

} // namespace Moer
