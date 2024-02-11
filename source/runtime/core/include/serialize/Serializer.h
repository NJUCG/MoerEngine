#ifndef MOER_SERIALIZER_H
#define MOER_SERIALIZER_H
#include "zpp_bits.h"

namespace Moer {
    // class Serializer {
    // public:
    //     Serializer()                             = default;
    //     ~Serializer()                            = default;
    //     Serializer(const Serializer&)            = delete;
    //     Serializer& operator=(const Serializer&) = delete;
    //     Serializer(Serializer&&)                 = delete;
    //     Serializer& operator=(Serializer&&)      = delete;

    //     template<typename T>
    //     void Serialize(T& _value) {
    //         Serialize(&_value, sizeof(T));
    //     }

    //     template<typename T>
    //     void Serialize(T* _value, size_t _size) {
    //         // if (is_reading) {
    //         //     if (zip_bits::read_bytes(_value, _size, zip_bits::read_bytes_from_zip, &zip_file) != _size) {
    //         //         assert(false);
    //         //     }
    //         // } else {
    //         //     if (zip_bits::write_bytes(_value, _size, zip_bits::write_bytes_to_zip, &zip_file) != _size) {
    //         //         assert(false);
    //         //     }
    //         // }
    //     }

    // private:
    //     void Serialize(void* _value, size_t _size);
    // };
}// namespace Moer

#endif