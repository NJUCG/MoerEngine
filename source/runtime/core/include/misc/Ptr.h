#ifndef MOREENGINE_PTR_H
#define MOREENGINE_PTR_H
#include <cstdint>
template<typename TPtr, uint32_t Alignment>
class alignas(Alignment) AlignedPtr {
public:
    AlignedPtr() {}

    AlignedPtr(const TPtr& Other)
        : ref(Other) {}

    AlignedPtr(const AlignedPtr<TPtr, Alignment>& Other)
        : ref(Other.ref) {}

    inline void operator=(const TPtr& Other) {
        ref = Other;
    }

    inline operator TPtr&() {
        return ref;
    }

    inline operator const TPtr&() const {
        return ref;
    }

    inline const TPtr& operator->() const {
        return ref;
    }

protected:
    TPtr ref;
};

#endif