#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace Moer::Render::RenderGraph {

class FLinearArena {
public:
    static constexpr size_t DEFAULT_PAGE_SIZE = 2 * 1024 * 1024;

    FLinearArena(size_t InPageSize = DEFAULT_PAGE_SIZE) : PageSize(InPageSize) {
        HeadPage    = CreatePage(PageSize);
        CurrentPage = HeadPage;
    }

    ~FLinearArena() {
        DestroyAllPages();
    }

    FLinearArena(const FLinearArena&)            = delete;
    FLinearArena& operator=(const FLinearArena&) = delete;

    void* Allocate(size_t Size, size_t Align) {
        void* Ptr = TryAllocateFromPage(CurrentPage, Size, Align);
        if (Ptr)
            return Ptr;

        return AllocateSlow(Size, Align);
    }

    void Reset() {
        CurrentPage = HeadPage;
        FPage* P    = HeadPage;
        while (P) {
            P->Offset = 0;
            P         = P->Next;
        }
    }

    void DestroyAllPages() {
        FPage* P = HeadPage;
        while (P) {
            FPage* Next = P->Next;
            std::free(P);
            P = Next;
        }
        HeadPage = CurrentPage = nullptr;
    }

private:
    struct FPage {
        uint8_t* Data;
        size_t   Size;
        size_t   Offset;
        FPage*   Next;
    };

    FPage* CreatePage(size_t InSize) {
        static constexpr size_t Alignment = 16;

        // manually align
        size_t TotalSize = sizeof(FPage) + InSize + Alignment;

        uint8_t* RawMem = (uint8_t*)std::malloc(TotalSize);
        assert(RawMem && "Out of memory");

        FPage* NewPage = reinterpret_cast<FPage*>(RawMem);

        uintptr_t StructEnd   = reinterpret_cast<uintptr_t>(RawMem + sizeof(FPage));
        uintptr_t AlignedData = (StructEnd + (Alignment - 1)) & ~(Alignment - 1);

        NewPage->Next   = nullptr;
        NewPage->Data   = reinterpret_cast<uint8_t*>(AlignedData);
        NewPage->Size   = InSize;
        NewPage->Offset = 0;

        return NewPage;
    }

    void* TryAllocateFromPage(FPage* Page, size_t Size, size_t Align) {
        uintptr_t CurrPtr    = reinterpret_cast<uintptr_t>(Page->Data + Page->Offset);
        uintptr_t AlignedPtr = (CurrPtr + (Align - 1)) & ~(Align - 1);

        size_t NewOffset = (AlignedPtr - reinterpret_cast<uintptr_t>(Page->Data)) + Size;

        if (NewOffset <= Page->Size) {
            Page->Offset = NewOffset;
            return reinterpret_cast<void*>(AlignedPtr);
        }
        return nullptr;
    }

    void* AllocateSlow(size_t Size, size_t Align) {
        if (CurrentPage->Next) {
            CurrentPage         = CurrentPage->Next;
            CurrentPage->Offset = 0;
        } else {
            size_t NewSize = std::max(Size + Align, PageSize);
            FPage* NewPage = CreatePage(NewSize);

            CurrentPage->Next = NewPage;
            CurrentPage       = NewPage;
        }
        return TryAllocateFromPage(CurrentPage, Size, Align);
    }

    const size_t PageSize;
    FPage*       HeadPage;
    FPage*       CurrentPage;
};

} // namespace Moer::Render::RenderGraph
