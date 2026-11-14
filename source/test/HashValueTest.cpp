#include "misc/Hash.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {

void Expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void CheckHashValues() {
    SHA256Hash sha;
    Hash64City city;
    Expect(std::ranges::all_of(sha.hash_code, [](auto byte) {
               return byte == 0;
           }),
           "SHA starts empty");
    Expect(city == Hash64City{}, "CityHash starts empty");

    Hash64City lower;
    Hash64City higher;
    lower.hash_code[0]  = 0x7f;
    higher.hash_code[0] = 0x80;
    Expect(lower < higher && lower != higher, "Digest ordering uses unsigned bytes");
    Expect(higher == Hash64City(higher), "Digest equality compares bytes");
    sha.hash_code.back() = 1;
    Expect(sha != SHA256Hash{}, "SHA equality includes the final byte");

    std::string_view text("a\0b", 3);
    Hash64City       from_bytes;
    city.FromString(text);
    from_bytes.FromData(reinterpret_cast<const Moer::uint8*>(text.data()), text.size());
    Expect(city == from_bytes, "Hash entry points preserve embedded nulls");

    Hash64City from_text  = city;
    Hash64City from_chars = city;
    city.Update(text);
    from_text.Update(reinterpret_cast<const Moer::uint8*>(text.data()), static_cast<Moer::uint32>(text.size()));
    from_chars.Update(text.data(), static_cast<Moer::uint32>(text.size()));
    Expect(city == from_text && city == from_chars, "Seeded hash entry points agree");

    Expect(GetHash(1.0f) == 0x3f800000u, "Float hashing retains its bit representation");
    Expect(GetHash(-0.0f) == 0x80000000u, "Float hashing retains the sign bit");
    Expect(GetHash(1.0) == GetHash(Moer::uint64{0x3ff0000000000000}), "Double hashing retains its bit representation");
}

} // namespace

int main() {
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    try {
        CheckHashValues();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
