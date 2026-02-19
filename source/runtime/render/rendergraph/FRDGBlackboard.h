#include "RenderGraphDefinitions.h"

namespace Moer::Render::RenderGraph {

/** Declares a struct for use by the RDG blackboard. */
#define RDG_REGISTER_BLACKBOARD_STRUCT(StructType)                       \
    template<>                                                           \
    inline FString FRDGBlackboard::GetTypeName<StructType>() {           \
        return GetTypeName(TEXT(#StructType), TEXT(__FILE__), __LINE__); \
    }

class FRDGBlackboard {
public:
    /** Creates a new instance of a struct. Asserts if one already existed. */
    template<typename StructType, typename... ArgsType>
    StructType& Create(ArgsType&&... Args) {
        using HelperStructType = TStruct<StructType>;

        const int32 StructIndex = GetStructIndex<StructType>();
        if (StructIndex >= Blackboard.Num()) {
            Blackboard.SetNumZeroed(StructIndex + 1);
        }

        assert(
            !Blackboard[StructIndex],
            TEXT(
                "RDGBlackboard duplicate Create called on struct '%s'. Only one Create call per struct is "
                "allowed."
            ),
            GetTypeName<StructType>()
        );
        FStruct* Result = Allocator.Alloc<HelperStructType>(Forward<ArgsType&&>(Args)...);
        assert(Result);
        Blackboard[StructIndex] = Result;
        return static_cast<HelperStructType*>(Result)->Struct;
    }

    /** Gets a mutable instance of the struct. Returns null if not present in the blackboard. */
    template<typename StructType>
    StructType* GetMutable() const {
        using HelperStructType = TStruct<StructType>;

        const int32 StructIndex = GetStructIndex<StructType>();
        if (StructIndex < Blackboard.Num()) {
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
        return Create<StructType>(Forward<ArgsType&&>(Args)...);
    }

    /** Gets a mutable instance of the struct. Asserts if not present in the blackboard. */
    template<typename StructType>
    StructType& GetMutableChecked() const {
        StructType* Struct = GetMutable<StructType>();
        assert(
            Struct,
            TEXT("RDGBlackboard Get failed to find instance of struct '%s' in the blackboard."),
            GetTypeName<StructType>()
        );
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
        Blackboard.Empty();
    }

    struct FStruct {
        virtual ~FStruct() = default;
    };

    template<typename StructType>
    struct TStruct final : public FStruct {
        template<typename... TArgs>
        inline TStruct(TArgs&&... Args) : Struct(Forward<TArgs&&>(Args)...) {}

        StructType Struct;
    };

    template<typename StructType>
    static std::string GetTypeName() {
        // Forces the compiler to only evaluate the assert on a concrete type.
        static_assert(
            sizeof(StructType) == 0,
            "Struct has not been registered with the RDG blackboard. Use RDG_REGISTER_BLACKBOARD_STRUCT to "
            "do this."
        );
        return std::string();
    }

    static RENDER_API std::string
                      GetTypeName(const TCHAR* ClassName, const TCHAR* FileName, uint32 LineNumber) {
        char buffer[512];
        snprintf(buffer, sizeof(buffer), "%s %s %u", ClassName, FileName, LineNumber);
        return std::string(buffer);
    }

    static RENDER_API uint32 AllocateIndex(std::string&& TypeName) {
        static Map<std::string, uint32> StructMap;
        static uint32                   NextIndex = 0;

        uint32 Result;
        if (const uint32* FoundIndex = StructMap.Find(TypeName)) {
            Result = *FoundIndex;
        } else {
            StructMap.Emplace(std::move(TypeName), NextIndex);
            Result = NextIndex;
            NextIndex++;
        }
        return Result;
    }

    template<typename StructType>
    static uint32 GetStructIndex() {
        static uint32 Index = UINT_MAX;
        if (Index == UINT_MAX) {
            Index = AllocateIndex(GetTypeName<StructType>());
        }
        return Index;
    }

    FRDGAllocator&                      Allocator;
    Array<FStruct*, FRDGArrayAllocator> Blackboard;

    friend class FRDGBuilder;
};
} // namespace Moer::Render::RenderGraph