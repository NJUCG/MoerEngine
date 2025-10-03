#include "scene/EntityManager.h"
namespace Moer {
    void EntityManager::Create(size_t n, Entity* entities) noexcept {
        Entity::Type index{};
        auto& freeList = mFreeList;
        uint8_t* const gens = m_gens;

        std::lock_guard<std::mutex> lock(mFreeListLock);
        Entity::Type currentIndex = mCurrentIndex;
        for (size_t i = 0; i < n; i++) {

            if (currentIndex >= RAW_INDEX_COUNT || freeList.size() >= MIN_FREE_INDICES) {
                index = freeList.front();
                freeList.pop_front();
            } else {
                index = currentIndex++;
            }
            entities[i] = Entity{ makeIdentity(gens[index], index) };
        }
        mCurrentIndex = currentIndex;
    }
    void EntityManager::Destroy(size_t n, Entity* entities) noexcept {
        auto& freeList = mFreeList;
        uint8_t* const gens = m_gens;

        std::unique_lock<std::mutex> lock(mFreeListLock);
        for (size_t i = 0; i < n; i++) {
            if (!entities[i]) {
                continue;
            }
        
            assert(IsAlive(entities[i]));
        
            if (IsAlive(entities[i])) {
                Entity::Type index = getIndex(entities[i]);
                freeList.push_back(index);
                gens[index]++;
            }
        }
    }
    Entity EntityManager::Create() noexcept {
        Entity entity;
        Create(1,&entity);
        return  entity;
    }
    void EntityManager::Destroy(Entity entity) noexcept {
        Destroy(1,&entity);
    }

    bool EntityManager::IsAlive(Entity entity) const noexcept {
        assert(getIndex(entity) < RAW_INDEX_COUNT);
        return (!entity.isNull()) && (getGeneration(entity) == m_gens[getIndex(entity)]);
    }

    EntityManager& EntityManager::Get() noexcept {
        if (m_instance == nullptr) {
            m_instance = new EntityManager();
        }
        return *m_instance;
    }


    EntityManager::EntityManager() noexcept : m_gens(new uint8_t[RAW_INDEX_COUNT]) {
        std::fill_n(m_gens, RAW_INDEX_COUNT, 0);
    }
    EntityManager::~EntityManager() noexcept {
        delete [] m_gens;
    }
}