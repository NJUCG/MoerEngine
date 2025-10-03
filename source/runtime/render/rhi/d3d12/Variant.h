#ifndef VARIANT_H
#define VARIANT_H

#pragma once

#include <variant>

template<typename... Types>
class Variant : public std::variant<Types...> {
public:
    using StdVariant = std::variant<Types...>;

    using std::variant<Types...>::variant;

    Variant(const StdVariant& other) : StdVariant(other) {}

    template<typename T>
    bool Is() const noexcept {
        return std::holds_alternative<T>(*this);
    }

    template<typename T>
    auto& As() const noexcept {
        return std::get<T>(*this);
    }

    template<typename T>
    auto& As() noexcept {
        return std::get<T>(*this);
    }

    template<typename T>
    Variant& operator=(T&& rhs) {
        *static_cast<StdVariant*>(this) = std::forward<T>(rhs);
        return *this;
    }

    template<typename T>
    auto AsIf() noexcept {
        return std::get_if<T>(this);
    }

    template<typename T>
    auto AsIf() const noexcept {
        return std::get_if<T>(this);
    }

    template<typename... Vs>
    auto Match(Vs... vs);

    template<typename... Vs>
    auto Match(Vs... vs) const;

    auto operator<=>(const Variant& rhs) const;

    bool operator==(const Variant&) const;
};

namespace VariantImpl {

template<typename T>
decltype(auto) ToStdVariant(T&& var) {
    return std::forward<T>(var);
}

template<typename... Ts>
std::variant<Ts...>& ToStdVariant(Variant<Ts...>& var) {
    return static_cast<std::variant<Ts...>&>(var);
}

template<typename... Ts>
const std::variant<Ts...>& ToStdVariant(const Variant<Ts...>& var) {
    return static_cast<const std::variant<Ts...>&>(var);
}

template<typename... Ts>
std::variant<Ts...> ToStdVariant(Variant<Ts...>&& var) {
    return static_cast<std::variant<Ts...>>(var);
}

} // namespace VariantImpl

template<typename E, typename... Vs>
auto MatchVariant(E&& e, Vs... vs) {
    struct Overloaded : Vs... {
        explicit Overloaded(Vs... vss) : Vs(vss)... {}
        using Vs::operator()...;
    };
    return std::visit(Overloaded(vs...), VariantImpl::ToStdVariant(std::forward<E>(e)));
}

template<typename... Types>
template<typename... Vs>
auto Variant<Types...>::Match(Vs... vs) {
    return MatchVariant(*this, std::move(vs)...);
}

template<typename... Types>
template<typename... Vs>
auto Variant<Types...>::Match(Vs... vs) const {
    return MatchVariant(*this, std::move(vs)...);
}

template<typename... Types>
auto Variant<Types...>::operator<=>(const Variant& rhs) const {
    return VariantImpl::ToStdVariant(*this) <=> VariantImpl::ToStdVariant(rhs);
}

template<typename... Types>
bool Variant<Types...>::operator==(const Variant& rhs) const {
    return VariantImpl::ToStdVariant(*this) == VariantImpl::ToStdVariant(rhs);
}

#endif // VARIANT_H