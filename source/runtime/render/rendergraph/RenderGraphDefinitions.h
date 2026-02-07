#pragma once
#include <cstdint>
#include <limits>

#include "RenderGraphAllocator.h"

namespace Moer {

enum class ERDGHandleRegistryDestructPolicy {
    None,
    Registry
};

template<
    typename LocalHandleType,
    ERDGHandleRegistryDestructPolicy DestructPolicy = ERDGHandleRegistryDestructPolicy::None>
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
    std::vector<uint64_t> Data;
    uint32_t              NumBits = 0;
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

class RDGPass;
using RDGPassHandle    = TRDGHandle<RDGPass, uint32>;
using RDGPassBitArray  = TRDGHandleBitArray<RDGPassHandle>;
using FRDGPassRegistry = TRDGHandleRegistry<FRDGPassHandle>;

} // namespace Moer
