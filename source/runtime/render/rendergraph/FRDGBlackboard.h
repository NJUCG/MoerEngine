#pragma once

#include <cassert>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

#include "RenderGraphDefinitions.h"

namespace Moer::Render::RenderGraph {

/** Declares a struct for use by the RDG blackboard. */
#define RDG_REGISTER_BLACKBOARD_STRUCT(StructType)                \
    template<>                                                    \
    inline std::string FRDGBlackboard::GetTypeName<StructType>() { \
        return GetTypeName(#StructType, __FILE__, __LINE__);     \
    }

class FRDGBlackboard {
public:
    /** Creates a new instance of a struct. Asserts if one already existed. */
    template<typename StructType, typename... ArgsType>
    StructType& Create(ArgsType&&... Args) {
        using HelperStructType = TStruct<StructType>;

        const size_t StructIndex = static_cast<size_t>(GetStructIndex<StructType>());
        if (StructIndex >= Blackboard.size()) {
            Blackboard.resize(StructIndex + 1, nullptr);
        }

        assert(
            !Blackboard[StructIndex] &&
            "RDGBlackboard duplicate Create called. Only one Create call per struct is allowed."
        );

        FStruct* Result = Allocator.Alloc<HelperStructType>(std::forward<ArgsType>(Args)...);
        assert(Result);

        Blackboard[StructIndex] = Result;
        return static_cast<HelperStructType*>(Result)->Struct;
    }

    /** Gets a mutable instance of the struct. Returns null if not present in the blackboard. */
    template<typename StructType>
    StructType* GetMutable() const {
        using HelperStructType = TStruct<StructType>;

        const size_t StructIndex = static_cast<size_t>(GetStructIndex<StructType>());
        if (StructIndex < Blackboard.size()) {
            if (HelperStructType* Element = static_cast<HelperStructType*>(Blackboard[StructIndex])) {
                return &Element->Struct;
            }
        }
        return nullptr;
    }

    /** Gets an immutable instance of the struct. Returns null if not present in the blackboard. */
    template<typename StructType>
    const StructType* Get() const {
        return GetMutable<StructType>();
    }

    template<typename StructType, typename... ArgsType>
    StructType& GetOrCreate(ArgsType&&... Args) {
        if (StructType* Struct = GetMutable<StructType>()) {
            return *Struct;
        }
        return Create<StructType>(std::forward<ArgsType>(Args)...);
    }

    /** Gets a mutable instance of the struct. Asserts if not present in the blackboard. */
    template<typename StructType>
    StructType& GetMutableChecked() const {
        StructType* Struct = GetMutable<StructType>();
        assert(Struct && "RDGBlackboard Get failed to find struct instance in blackboard.");
        return *Struct;
    }

    /** Gets an immutable instance of the struct. Asserts if not present in the blackboard. */
    template<typename StructType>
    const StructType& GetChecked() const {
        return GetMutableChecked<StructType>();
    }

private:
    FRDGBlackboard(FRDGAllocator& InAllocator) : Allocator(InAllocator) {}

    void Clear() {
        Blackboard.clear();
    }

    struct FStruct {
        virtual ~FStruct() = default;
    };

    template<typename StructType>
    struct TStruct final : public FStruct {
        template<typename... TArgs>
        explicit TStruct(TArgs&&... Args) : Struct(std::forward<TArgs>(Args)...) {}

        StructType Struct;
    };

    template<typename StructType>
    static std::string GetTypeName() {
        // Forces the compiler to only evaluate the static_assert on a concrete type.
        static_assert(
            sizeof(StructType) == 0,
            "Struct has not been registered with the RDG blackboard. Use RDG_REGISTER_BLACKBOARD_STRUCT."
        );
        return {};
    }

    static std::string GetTypeName(const char* ClassName, const char* FileName, uint32 LineNumber) {
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer), "%s %s %u", ClassName, FileName, LineNumber);
        return std::string(buffer);
    }

    static uint32 AllocateIndex(std::string&& TypeName) {
        static Map<std::string, uint32> StructMap;
        static uint32                   NextIndex = 0;

        if (auto FoundIndexIt = StructMap.find(TypeName); FoundIndexIt != StructMap.end()) {
            return FoundIndexIt->second;
        }

        const uint32 Result = NextIndex++;
        StructMap.emplace(std::move(TypeName), Result);
        return Result;
    }

    template<typename StructType>
    static uint32 GetStructIndex() {
        static uint32 Index = std::numeric_limits<uint32>::max();
        if (Index == std::numeric_limits<uint32>::max()) {
            Index = AllocateIndex(GetTypeName<StructType>());
        }
        return Index;
    }

    FRDGAllocator&                      Allocator;
    Array<FStruct*, FRDGArrayAllocator> Blackboard;

    friend class FRDGBuilder;
};
} // namespace Moer::Render::RenderGraph
