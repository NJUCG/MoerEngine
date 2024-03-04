#pragma once
#include <memory>
#include <unordered_map>

#include "rhi/RHIResource.h"

// class Transform;
namespace Moer {
    class Entity {
    public:
        using Type = uint32_t;

        Entity() noexcept {}

        // Entities can be copied
        Entity(const Entity& e) noexcept            = default;
        Entity(Entity&& e) noexcept                 = default;
        Entity& operator=(const Entity& e) noexcept = default;
        Entity& operator=(Entity&& e) noexcept      = default;

        // Entities can be compared
        bool operator==(Entity e) const { return e.mIdentity == mIdentity; }
        bool operator!=(Entity e) const { return e.mIdentity != mIdentity; }

        // Entities can be sorted
        bool operator<(Entity e) const { return e.mIdentity < mIdentity; }

        bool isNull() const noexcept {
            return mIdentity == 0;
        }

        // an id that can be used for debugging/printing
        uint32_t getId() const noexcept {
            return mIdentity;
        }

        explicit operator bool() const noexcept { return !isNull(); }

        void clear() noexcept { mIdentity = 0; }

        struct Hasher {
            typedef Entity argument_type;
            typedef size_t result_type;
            result_type    operator()(argument_type const& e) const {
                return e.getId();
            }
        };

    public:
        friend class EntityManager;
        friend class EntityManagerImpl;
        using Type = uint32_t;

        explicit Entity(Type identity) noexcept : mIdentity(identity) {}

        Type mIdentity = 0;
    };
}// namespace Moer
