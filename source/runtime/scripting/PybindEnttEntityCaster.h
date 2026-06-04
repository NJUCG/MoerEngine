#pragma once

#include <entt/entt.hpp>
#include <pybind11/pybind11.h>

#include <type_traits>

namespace pybind11::detail {

template<>
struct type_caster<entt::entity> {
    using UnderlyingType = std::underlying_type_t<entt::entity>;

    PYBIND11_TYPE_CASTER(entt::entity, _("int"));

    bool load(handle src, bool) {
        if (!src) {
            return false;
        }

        PyObject* index_value = PyNumber_Index(src.ptr());
        if (index_value == nullptr) {
            PyErr_Clear();
            return false;
        }

        object owned_index = reinterpret_steal<object>(index_value);
        try {
            value = static_cast<entt::entity>(owned_index.cast<UnderlyingType>());
            return true;
        } catch (const pybind11::cast_error&) {
            PyErr_Clear();
            return false;
        }
    }

    static handle cast(entt::entity src, return_value_policy, handle) {
        return pybind11::int_(static_cast<UnderlyingType>(entt::to_integral(src))).release();
    }
};

} // namespace pybind11::detail