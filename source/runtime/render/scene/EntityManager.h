#pragma once

#include <deque>

#include "Entity.h"
namespace Moer {
class RENDER_API EntityManager {

public:
    void   Create(size_t n, Entity* entities) noexcept;
    void   Destroy(size_t n, Entity* entities) noexcept;
    Entity Create() noexcept;
    void   Destroy(Entity entity) noexcept;
    bool   IsAlive(Entity entity) const noexcept;

    static EntityManager& Get() noexcept;

private:
    EntityManager() noexcept;
    ~EntityManager() noexcept;

    static constexpr const int          GENERATION_SHIFT = 17;
    static constexpr const size_t       RAW_INDEX_COUNT  = (1 << GENERATION_SHIFT);
    static constexpr const Entity::Type INDEX_MASK       = (1 << GENERATION_SHIFT) - 1u;
    static constexpr const size_t       MIN_FREE_INDICES = 1024;

    static inline Entity::Type getGeneration(Entity e) noexcept {
        return e.getId() >> GENERATION_SHIFT;
    }
    static inline Entity::Type getIndex(Entity e) noexcept {
        return e.getId() & INDEX_MASK;
    }
    static inline Entity::Type makeIdentity(Entity::Type g, Entity::Type i) noexcept {
        return (g << GENERATION_SHIFT) | (i & INDEX_MASK);
    }

private:
    uint8_t* const m_gens;
    uint32_t       mCurrentIndex = 1;

    // stores indices that got freed
    mutable std::mutex       mFreeListLock;
    std::deque<Entity::Type> mFreeList;

    static inline EntityManager* m_instance = nullptr;
};
} // namespace Moer