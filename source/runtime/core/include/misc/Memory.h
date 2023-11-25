#ifndef MOER_ENGINE_MEMORY_H
#define MOER_ENGINE_MEMORY_H
#include <memory>

namespace Moer {
    class StackAllocator {
    public:
        StackAllocator(size_t _size);
        ~StackAllocator();
        void*  Allocate(size_t _size);
        void   Free(void* _ptr);
        void   Reset();
        void   Clear();
        size_t GetSize() const { return size; }
        size_t GetUsedSize() const { return used_size; }
        size_t GetFreeSize() const { return size - used_size; }

    private:
        size_t size;
        size_t used_size;
    };
}// namespace Moer
#endif//MOER_ENGINE_MEMORY_H