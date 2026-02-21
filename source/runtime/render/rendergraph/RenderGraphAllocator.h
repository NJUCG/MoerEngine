#include <cassert>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

#include "RenderGraphArena.h"
#include "misc/Traits.h"
namespace Moer::Render::RenderGraph {

class FRDGAllocator {
public:
    class FObject {
    public:
        virtual ~FObject() = default;
    };

    template<typename T>
    class TObject final : public FObject {
    public:
        template<typename... TArgs>
        TObject(TArgs&&... Args) : Obj(std::forward<TArgs>(Args)...) {}
        ~TObject() override = default;
        T Obj;
    };

    FRDGAllocator(FLinearArena& InArena) : Arena(InArena) {}
    ~FRDGAllocator() {
        ReleaseAll();
    }

    FRDGAllocator(const FRDGAllocator&)            = delete;
    FRDGAllocator& operator=(const FRDGAllocator&) = delete;

    void* AllocRaw(size_t Size, size_t Align) {
        return Arena.Allocate(Size, Align);
    }

    // Suitable for non-POD with destructor
    template<typename T, typename... TArgs>
    T* Alloc(TArgs&&... Args) {
        void* Mem     = AllocRaw(sizeof(TObject<T>), alignof(TObject<T>));
        auto* Wrapper = new (Mem) TObject<T>(std::forward<TArgs>(Args)...);
        Objects.push_back(Wrapper);
        return &Wrapper->Obj;
    }

    // Suitable for POD
    template<typename T, typename... TArgs>
    T* AllocNoDestruct(TArgs&&... Args) {
        void* Mem = AllocRaw(sizeof(T), alignof(T));
        return new (Mem) T(std::forward<TArgs>(Args)...);
    }

    // Allocate uninitialized POD (single element)
    template<typename T>
    T* AllocUninitialized() {
        return static_cast<T*>(AllocRaw(sizeof(T), alignof(T)));
    }

    // Allocate uninitialized POD array
    template<typename T>
    T* AllocUninitialized(size_t Count) {
        return static_cast<T*>(AllocRaw(sizeof(T) * Count, alignof(T)));
    }

    // Raw allocation with size and alignment (used by AllocPODArray etc.)
    void* Alloc(size_t SizeInBytes, size_t AlignInBytes) {
        return AllocRaw(SizeInBytes, AlignInBytes);
    }

    void ReleaseAll() {
        for (int i = (int)Objects.size() - 1; i >= 0; --i) {
            Objects[i]->~FObject();
        }
        Objects.clear();
    }

private:
    FLinearArena&   Arena;
    Array<FObject*> Objects;
};

//A simple version of array allocator
template<typename T>
class TRDGAdapter {
public:
    using value_type = T;

    explicit TRDGAdapter(FRDGAllocator& InAlloc) : Allocator(InAlloc) {}

    // 必须有这个，否则 std::list 或复杂的嵌套容器无法工作AllocNoDestruct
    template<typename U>
    struct rebind {
        using other = TRDGAdapter<U>;
    };

    T* allocate(std::size_t n) {
        return static_cast<T*>(Allocator.AllocRaw(n * sizeof(T), alignof(T)));
    }

    //just do nothing
    void deallocate(T* p, std::size_t n) noexcept {}

    template<typename U>
    TRDGAdapter(const TRDGAdapter<U>& Other) : Allocator(Other.Allocator) {}

    FRDGAllocator& Allocator;
};

class FRDGAllocatorScope {
public:
    RENDER_API FRDGAllocatorScope(FRDGAllocator& Allocator);
    RENDER_API ~FRDGAllocatorScope();

private:
    void* AllocatorToRestore;
};

template<typename T>
using FRDGArrayAllocator = TRDGAdapter<T>;

} // namespace Moer::Render::RenderGraph
