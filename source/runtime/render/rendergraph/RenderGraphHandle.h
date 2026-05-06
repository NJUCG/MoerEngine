#pragma once
#include <cstdint>
#include <limits>
namespace Moer::Render {
class RenderGraphHandle {
public:
    using Index   = uint16_t;
    using Version = uint16_t;

    struct Hash {
        std::size_t operator()(const RenderGraphHandle& handle) const noexcept {
            return handle.index << 16 | handle.version;
        }
    };

    // std::size_t getHash() const;

    // private:
    explicit RenderGraphHandle(Index index) noexcept : index(index) {}

    // index to the resource handle
    static constexpr uint16_t uninitialized = std::numeric_limits<Index>::max();

    Index   index   = uninitialized; // index to a ResourceSlot
    Version version = 0;

    // protected:
    // private ctor -- this cannot be constructed by users
    RenderGraphHandle() noexcept = default;

    // friend class RenderGraph;
    //
    // friend class Blackboard;

public:
    RenderGraphHandle(const RenderGraphHandle& rhs) noexcept = default;

    RenderGraphHandle& operator=(const RenderGraphHandle& rhs) {
        index   = rhs.index;
        version = rhs.version;
        return *this;
    }

    bool IsInitialized() const noexcept {
        return index != uninitialized;
    }

    operator bool() const noexcept {
        return IsInitialized();
    }

    void Clear() noexcept {
        index   = uninitialized;
        version = 0;
    }

    bool operator<(const RenderGraphHandle& rhs) const noexcept {
        return index < rhs.index;
    }

    bool operator==(const RenderGraphHandle& rhs) const noexcept {
        return (index == rhs.index);
    }

    bool operator!=(const RenderGraphHandle& rhs) const noexcept {
        return !operator==(rhs);
    }
};
} // namespace Moer::Render
