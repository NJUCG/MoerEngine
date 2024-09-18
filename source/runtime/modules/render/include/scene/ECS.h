#pragma once

#include "Entity.h"

namespace Moer {
    template<typename COMPONENT>
    class EntityComponentManger {
    public:
        EntityComponentManger() noexcept {
            // We always start with a dummy entry because index=0 is reserved. The component
            // at index = 0, is guaranteed to be default-initialized.
            // Sub-classes can use this to their advantage.
            m_data.push_back(MoerNew(COMPONENT));
        }

        EntityComponentManger(EntityComponentManger&&) noexcept            = default;
        EntityComponentManger& operator=(EntityComponentManger&&) noexcept = default;
        ~EntityComponentManger() noexcept {
            m_instance_map.clear();
            m_data.clear();
        };

        // not copyable
        EntityComponentManger(EntityComponentManger const& rhs)            = delete;
        EntityComponentManger& operator=(EntityComponentManger const& rhs) = delete;

        bool       HasComponent(Entity entity) const;
        void       AddComponent(Entity entity);
        void       RemoveComponent(Entity entity);
        COMPONENT& operator[](Entity entity);

        using Instance = uint16_t;

    private:
        std::unordered_map<Entity, Instance, Entity::Hasher> m_instance_map;
        Moer::Array<COMPONENT*>                              m_data;
    };

    template<typename COMPONENT>
    bool EntityComponentManger<COMPONENT>::HasComponent(Entity entity) const {
        return m_instance_map.contains(entity);
    }

    template<typename COMPONENT>
    void EntityComponentManger<COMPONENT>::AddComponent(Entity entity) {
        // TODO: Should we assert or log a warning that the entity already has a component?
        // if (m_instance_map.contains(entity)) assert(false); // or do something similar
        m_instance_map.emplace(entity, m_data.size());
        m_data.push_back(MoerNew(COMPONENT));
    }

    template<typename COMPONENT>
    void EntityComponentManger<COMPONENT>::RemoveComponent(Entity entity) {
        MoerDelete(m_data[m_instance_map[entity]]);
        m_data[m_instance_map[entity]] = std::move(m_data.back());
        m_data.pop_back();
    }

    template<typename COMPONENT>
    COMPONENT& EntityComponentManger<COMPONENT>::operator[](Entity entity) {
        return *m_data[m_instance_map[entity]];
    }

    template<typename T>
    class DLLEXPORT PrivateImplementation {

    public:
        // none of these methods must be implemented inline because it's important that their
        // implementation be hidden from the public headers.
        template<typename... ARGS>
        explicit PrivateImplementation(ARGS&&...) noexcept;
        PrivateImplementation() noexcept;
        ~PrivateImplementation() noexcept;
        PrivateImplementation(PrivateImplementation const& rhs) noexcept;
        PrivateImplementation& operator=(PrivateImplementation const& rhs) noexcept;

        // move ctor and copy operator can be implemented inline and don't need to be exported
        PrivateImplementation(PrivateImplementation&& rhs) noexcept : m_impl(rhs.m_impl) { rhs.m_impl = nullptr; }
        PrivateImplementation& operator=(PrivateImplementation&& rhs) noexcept {
            auto temp  = m_impl;
            m_impl     = rhs.m_impl;
            rhs.m_impl = temp;
            return *this;
        }

    protected:
        T*              m_impl = nullptr;
        inline T*       operator->() noexcept { return m_impl; }
        inline T const* operator->() const noexcept { return m_impl; }
    };

    template<typename T>
    PrivateImplementation<T>::PrivateImplementation() noexcept
        : m_impl(new T) {
    }

    template<typename T>
    template<typename... ARGS>
    PrivateImplementation<T>::PrivateImplementation(ARGS&&... args) noexcept
        : m_impl(new T(std::forward<ARGS>(args)...)) {
    }

    template<typename T>
    PrivateImplementation<T>::~PrivateImplementation() noexcept {
        delete m_impl;
    }

    template<typename T>
    PrivateImplementation<T>::PrivateImplementation(PrivateImplementation const& rhs) noexcept
        : m_impl(new T(*rhs.m_impl)) {
    }

    template<typename T>
    PrivateImplementation<T>& PrivateImplementation<T>::operator=(PrivateImplementation<T> const& rhs) noexcept {
        if (this != &rhs) {
            *m_impl = *rhs.m_impl;
        }
        return *this;
    }
}// namespace Moer
