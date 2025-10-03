#ifndef MOER_SERIALIZER_H
#define MOER_SERIALIZER_H
#include "misc/STL.h"
#include "misc/Traits.h"
#include <cassert>
#include <cstddef>
#include <istream>
#include <optional>
#include <ostream>
#include <span>

namespace Moer {
template<typename T>
static constexpr bool is_ptr_t_v =
    is_shared_ptr_v<T> || is_unique_ptr_v<T> || is_countable_v<T> || std::is_pointer_v<T>;
static_assert(is_ptr_t_v<SharedPtr<uint>>);
struct InputStream {
    InputStream(std::istream& _stream) : m_stream(_stream) {}
    template<typename T>
    struct HasInputStreamOverloadFunction {
        template<typename U>
        static auto Test(U* _p
        ) -> decltype(std::declval<U>().operator>>(std::declval<InputStream&>()), std::true_type{});
        static auto Test(...) -> std::false_type;

        static constexpr bool value = decltype(Test(static_cast<T*>(nullptr)))::value || is_shared_ptr_v<T> ||
                                      is_unique_ptr_v<T> || is_countable_v<T>;
    };

    template<typename T>
    InputStream& operator>>(T& _value) {
        if constexpr (HasInputStreamOverloadFunction<T>::value) {
            if constexpr (is_ptr_t_v<T>) {
                bool has_value;
                *this >> has_value;
                if (has_value) {
                    if constexpr (is_shared_ptr_v<T>) {
                        T value = MakeShared<typename T::element_type>();
                        *this >> *value;
                        _value = value;
                    } else if constexpr (is_unique_ptr_v<T>) {
                        T value = MakeUnique<typename T::element_type>();
                        *this >> *value;
                        _value = value;
                    } else if constexpr (is_countable_v<T>) {
                        T value = MoerNew(typename T::CountableType)();
                        *this >> *value;
                        _value = value;
                    } else {
                        using U = std::remove_pointer_t<T>;
                        T value = MoerNew(U)();
                        *this >> *value;
                        _value = value;
                    }
                } else {
                    _value = nullptr;
                }
                return *this;
            } else
                return _value.operator>>(*this);
        } else {
            m_stream.read(reinterpret_cast<char*>(&_value), sizeof(T));
        }
        return *this;
    }

    template<>
    InputStream& operator>>(std::string& _value) {
        size_t size;
        *this >> size;
        _value.resize(size);
        m_stream.read(&_value[0], size);
        return *this;
    }

    template<>
    InputStream& operator>>(std::string_view& _value) {
        size_t size;
        *this >> size;
        assert(size <= _value.size());
        m_stream.read(const_cast<char*>(_value.data()), size);
        return *this;
    }

    //Array
    template<typename T>
    InputStream& operator>>(Moer::Array<T>& _value) {
        size_t size;
        *this >> size;
        if (size == 0) {
            return *this;
        }
        _value.resize(size);
        if constexpr (HasInputStreamOverloadFunction<T>::value) {
            for (auto& v : _value) {
                *this >> v;
            }
        } else {
            m_stream.read(reinterpret_cast<char*>(_value.data()), size * sizeof(T));
        }
        return *this;
    }

    template<typename T, size_t N>
    InputStream& operator>>(Moer::StaticArray<T, N>& _value) {
        if constexpr (N == 0) {
            return *this;
        }
        if constexpr (HasInputStreamOverloadFunction<T>::value) {
            for (auto& v : _value) {
                v.operator>>(*this);
            }
        } else {
            m_stream.read(reinterpret_cast<char*>(_value.data()), N * sizeof(T));
        }
        return *this;
    }

    //optional
    template<typename T>
    InputStream& operator>>(std::optional<T>& _value) {
        bool has_value;
        *this >> has_value;
        if (has_value) {
            T value;
            *this >> value;
            _value = value;
        } else {
            _value.reset();
        }
        return *this;
    }

    template<typename U, typename V>
    InputStream& operator>>(UnorderedMap<U, V>& _value) {
        size_t size;
        *this >> size;
        if (size == 0) {
            return *this;
        }
        for (size_t i = 0; i < size; ++i) {
            U key;
            V value;
            *this >> key >> value;
            _value.insert({key, value});
        }
        return *this;
    }

private:
    std::istream& m_stream;
};
static_assert(InputStream::HasInputStreamOverloadFunction<UniquePtr<uint>>::value);

struct OutputStream {
    template<typename T>
    struct HasOutputStreamOverloadFunction {
        template<typename U>
        static auto           Test(U* _p
                  ) -> decltype(std::declval<U>().operator<<(std::declval<OutputStream&>()), std::true_type{});
        static auto           Test(...) -> std::false_type;
        static constexpr bool value = decltype(Test(static_cast<T*>(nullptr)))::value || is_shared_ptr_v<T> ||
                                      is_unique_ptr_v<T> || is_countable_v<T>;
    };

    OutputStream(std::ostream& _stream) : m_stream(_stream) {}

    template<typename T>
    OutputStream& operator<<(const T& _value) {
        if constexpr (HasOutputStreamOverloadFunction<T>::value) {
            if constexpr (is_ptr_t_v<T>) {
                if (!_value)
                    return *this << false;

                return *this << true << *_value;
            } else {
                return _value.operator<<(*this);
            }
        } else {
            // m_stream << _value;
            m_stream.write(reinterpret_cast<const char*>(&_value), sizeof(T));
        }
        return *this;
    }

    template<>
    OutputStream& operator<<(const std::string& _value) {
        size_t size = _value.size();
        *this << size;
        m_stream.write(_value.c_str(), size);
        return *this;
    }

    template<>
    OutputStream& operator<<(const std::string_view& _value) {
        size_t size = _value.size();
        *this << size;
        m_stream.write(_value.data(), size);
        return *this;
    }

    template<typename T>
    OutputStream& operator<<(const Moer::Array<T>& _value) {
        *this << _value.size();
        if (_value.empty()) {
            return *this;
        }
        if constexpr (HasOutputStreamOverloadFunction<T>::value) {

            for (const auto& v : _value) {
                *this << v;
            }
        } else {
            m_stream.write(reinterpret_cast<const char*>(_value.data()), _value.size() * sizeof(T));
        }
        return *this;
    }

    template<typename T, size_t N>
    OutputStream& operator<<(const Moer::StaticArray<T, N>& _value) {
        if constexpr (N == 0) {
            return *this;
        }
        if constexpr (HasOutputStreamOverloadFunction<T>::value) {
            for (const auto& v : _value) {
                *this << v;
            }
        } else {
            m_stream.write(reinterpret_cast<const char*>(_value.data()), _value.size() * sizeof(T));
        }
        return *this;
    }
    //span
    template<typename T>
    OutputStream& operator<<(const std::span<T>& _value) {
        *this << _value.size();
        if (_value.empty()) {
            return *this;
        }

        if constexpr (HasOutputStreamOverloadFunction<T>::value) {
            for (const auto& v : _value) {
                *this << v;
            }
        } else {
            m_stream.write(reinterpret_cast<const char*>(_value.data()), _value.size() * sizeof(T));
        }

        return *this;
    }

    template<typename T>
    OutputStream& operator<<(const std::optional<T>& _value) {
        *this << _value.has_value();
        if (_value.has_value()) {
            *this << _value.value();
        }
        return *this;
    }

    template<typename T, typename U>
    OutputStream& operator<<(const UnorderedMap<T, U>& _value) {
        *this << _value.size();
        if (_value.empty()) {
            return *this;
        }
        for (const auto& [key, value] : _value) {
            *this << key << value;
        }
        return *this;
    }

private:
    std::ostream& m_stream;
};
} // namespace Moer

#endif