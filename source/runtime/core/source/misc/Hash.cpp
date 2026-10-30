#include "misc/Hash.h"
#include "CityHash.h"
#include "PicoSHA2.h"
#include "misc/Types.h"
#include <string>

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
    return oss.str().data();
}

void Hash64City::FromString(std::string_view& src) {
    auto* start = (Moer::uint64*)(&hash_code[0]);
    *start      = CityHash64(src.data(), src.size());
}
void Hash64City::FromData(const Moer::uint8* data, size_t size) {

    auto* start = (Moer::uint64*)(&hash_code[0]);
    *start      = CityHash64((const char*)data, size);
}
void Hash64City::Update(std::string_view& src) {
    Moer::uint64 src_seed           = *(Moer::uint64*)(&hash_code[0]);
    *(Moer::uint64*)(&hash_code[0]) = CityHash64WithSeed(src.data(), src.length(), src_seed);
}
void Hash64City::Update(const Moer::uint8* data, Moer::uint32 size) {
    Moer::uint64 src_seed           = *(Moer::uint64*)(&hash_code[0]);
    *(Moer::uint64*)(&hash_code[0]) = CityHash64WithSeed(reinterpret_cast<const char*>(data), size, src_seed);
}

void Hash64City::Update(const char* data, Moer::uint32 size) {
    Moer::uint64 src_seed           = *(Moer::uint64*)(&hash_code[0]);
    *(Moer::uint64*)(&hash_code[0]) = CityHash64WithSeed(data, size, src_seed);
}

std::atomic<Moer::uint32> HashedName::s_size = 0;
std::shared_mutex    HashedName::s_rw_mutex;