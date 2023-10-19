#include "misc/Hash.h"
#include "PicoSHA2.h"
#include "CityHash.h"

static_assert(picosha2::k_digest_size == 32);

std::string SHA256Hash::ToString() {
    return picosha2::bytes_to_hex_string(hash_code.begin(), hash_code.end());
}
void SHA256Hash::FromString(std::string_view& src) {
    picosha2::hash256(src, hash_code);
}

std::string Hash64City::ToString() {

    std::ostringstream oss;
    auto               first = hash_code.begin();
    auto               last  = hash_code.end();
    {
        oss.setf(std::ios::hex, std::ios::basefield);
        while (first != last) {
            oss.width(2);
            oss.fill('0');
            oss << static_cast<unsigned int>(*first);
            ++first;
        }
        oss.setf(std::ios::dec, std::ios::basefield);
    }
    return oss.str();
}

void Hash64City::FromString(std::string_view& src) {
    auto* start = (uint64_t*)(&hash_code[0]);
    *start      = CityHash64(src.data(), src.size());
}
void Hash64City::FromData(const uint8_t* data, size_t size) {

    auto* start = (uint64_t*)(&hash_code[0]);
    *start      = CityHash64((const char*)data, size);
}
void Hash64City::Update(std::string_view& src) {
    uint64_t src_seed           = *(uint64_t*)(&hash_code[0]);
    *(uint64_t*)(&hash_code[0]) = CityHash64WithSeed(src.data(), src.length(), src_seed);
}
void Hash64City::Update(const uint8_t* data, uint32_t size) {
    uint64_t src_seed           = *(uint64_t*)(&hash_code[0]);
    *(uint64_t*)(&hash_code[0]) = CityHash64WithSeed(reinterpret_cast<const char*>(data), size, src_seed);
}

void Hash64City::Update(const char* data, uint32_t size) {
    uint64_t src_seed           = *(uint64_t*)(&hash_code[0]);
    *(uint64_t*)(&hash_code[0]) = CityHash64WithSeed(data, size, src_seed);
}

std::atomic_uint32_t HashedName::s_size = 0;
std::shared_mutex    HashedName::s_rw_mutex;