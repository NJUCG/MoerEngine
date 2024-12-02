#ifndef MOER_SERIALIZER_H
#define MOER_SERIALIZER_H
#include "misc/STL.h"
#include "zpp_bits.h"
#include <cassert>
#include <ostream>
#include <istream>

namespace Moer {

    struct InputStream {
        InputStream(std::istream& _stream) : m_stream(_stream) {}
        template<typename T>
        struct HasInputStreamOverloadFunction {
            template<typename U>
            static auto           Test(U* _p) -> decltype(std::declval<U>().operator>>(std::declval<InputStream&>()), std::true_type{});
            static auto           Test(...) -> std::false_type;
            static constexpr bool value = decltype(Test(static_cast<T*>(nullptr)))::value;
        };

        template<typename T>
        InputStream& operator>>(T& _value) {
            if constexpr (HasInputStreamOverloadFunction<T>::value) {
                return _value >> *this;
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

    private:
        std::istream& m_stream;
    };

    struct OutputStream {
        template<typename T>
        struct HasOutputStreamOverloadFunction {
            template<typename U>
            static auto           Test(U* _p) -> decltype(std::declval<U>().operator<<(std::declval<OutputStream&>()), std::true_type{});
            static auto           Test(...) -> std::false_type;
            static constexpr bool value = decltype(Test(static_cast<T*>(nullptr)))::value;
        };

        OutputStream(std::ostream& _stream) : m_stream(_stream) {}

        template<typename T>
        OutputStream& operator<<(const T& _value) {
            if constexpr (HasOutputStreamOverloadFunction<T>::value) {
                return _value.operator<<(*this);
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
            for (const auto& v : _value) {
                *this << v;
            }
            return *this;
        }
        //span
        template<typename T>
        OutputStream& operator<<(const std::span<T>& _value) {
            *this << _value.size();
            for (const auto& v : _value) {
                *this << v;
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

    private:
        std::ostream& m_stream;
    };
}// namespace Moer

#endif