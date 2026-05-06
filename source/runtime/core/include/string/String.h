#pragma once

#include "misc/MMemory.h"
#include "string/CharTypes.h"

#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace Moer {

struct PlatformEncoding {};
struct Utf8Encoding {};
struct Utf16Encoding {};
struct Utf32Encoding {};
struct WideEncoding {};
struct AsciiEncoding {};

template<
    typename CharT,
    typename Encoding,
    typename Traits    = std::char_traits<CharT>,
    typename Allocator = MoerStlAllocator<CharT>>
class BasicString;

template<typename CharT, typename Encoding, typename Traits = std::char_traits<CharT>>
class BasicStringView {
public:
    using value_type  = CharT;
    using traits_type = Traits;
    using view_type   = std::basic_string_view<CharT, Traits>;

    static constexpr std::size_t npos = view_type::npos;

    constexpr BasicStringView() = default;
    constexpr BasicStringView(const CharT* text) : view(text) {}
    constexpr BasicStringView(const CharT* text, std::size_t size) : view(text, size) {}
    constexpr BasicStringView(view_type text) : view(text) {}

    template<typename Allocator>
    BasicStringView(const BasicString<CharT, Encoding, Traits, Allocator>& text) :
        view(text.data(), text.size()) {}

    constexpr const CharT* data() const noexcept {
        return view.data();
    }

    constexpr std::size_t size() const noexcept {
        return view.size();
    }

    constexpr bool empty() const noexcept {
        return view.empty();
    }

    constexpr const CharT* begin() const noexcept {
        return view.begin();
    }

    constexpr const CharT* end() const noexcept {
        return view.end();
    }

    constexpr CharT operator[](std::size_t index) const {
        return view[index];
    }

    constexpr view_type Native() const noexcept {
        return view;
    }

    constexpr std::size_t find(BasicStringView text, std::size_t pos = 0) const noexcept {
        return view.find(text.view, pos);
    }

    constexpr std::size_t find(CharT ch, std::size_t pos = 0) const noexcept {
        return view.find(ch, pos);
    }

    constexpr bool contains(BasicStringView text) const noexcept {
        return find(text) != npos;
    }

    constexpr bool starts_with(BasicStringView text) const noexcept {
        return view.starts_with(text.view);
    }

    constexpr bool ends_with(BasicStringView text) const noexcept {
        return view.ends_with(text.view);
    }

    constexpr BasicStringView substr(std::size_t pos, std::size_t count = npos) const noexcept {
        return BasicStringView(view.substr(pos, count));
    }

    constexpr operator view_type() const noexcept {
        return view;
    }

    constexpr bool operator==(BasicStringView other) const noexcept {
        return view == other.view;
    }

private:
    view_type view{};
};

template<typename CharT, typename Encoding, typename Traits, typename Allocator>
class BasicString {
public:
    using value_type       = CharT;
    using traits_type      = Traits;
    using allocator_type   = Allocator;
    using storage_type     = std::basic_string<CharT, Traits, Allocator>;
    using view_type        = BasicStringView<CharT, Encoding, Traits>;
    using native_view_type = std::basic_string_view<CharT, Traits>;

    static constexpr std::size_t npos = storage_type::npos;

    BasicString() = default;
    BasicString(const CharT* text) : storage(text) {}
    BasicString(native_view_type text) : storage(text) {}
    BasicString(view_type text) : storage(text.data(), text.size()) {}
    BasicString(storage_type text) : storage(std::move(text)) {}

    const CharT* c_str() const noexcept {
        return storage.c_str();
    }

    const CharT* data() const noexcept {
        return storage.data();
    }

    std::size_t size() const noexcept {
        return storage.size();
    }

    bool empty() const noexcept {
        return storage.empty();
    }

    CharT* data() noexcept {
        return storage.data();
    }

    CharT& operator[](std::size_t index) noexcept {
        return storage[index];
    }

    const CharT& operator[](std::size_t index) const noexcept {
        return storage[index];
    }

    auto begin() noexcept {
        return storage.begin();
    }

    auto end() noexcept {
        return storage.end();
    }

    auto begin() const noexcept {
        return storage.begin();
    }

    auto end() const noexcept {
        return storage.end();
    }

    void clear() noexcept {
        storage.clear();
    }

    void reserve(std::size_t capacity) {
        storage.reserve(capacity);
    }

    void resize(std::size_t size) {
        storage.resize(size);
    }

    void push_back(CharT value) {
        storage.push_back(value);
    }

    BasicString& append(view_type text) {
        storage.append(text.data(), text.size());
        return *this;
    }

    BasicString& append(const CharT* text) {
        storage.append(text);
        return *this;
    }

    BasicString& operator+=(const CharT* text) {
        storage += text;
        return *this;
    }

    BasicString& operator+=(view_type text) {
        storage.append(text.data(), text.size());
        return *this;
    }

    BasicString& operator+=(const BasicString& text) {
        storage += text.storage;
        return *this;
    }

    std::size_t find(view_type text, std::size_t pos = 0) const noexcept {
        return native_view_type(storage.data(), storage.size())
            .find(typename view_type::view_type(text), pos);
    }

    std::size_t find(CharT ch, std::size_t pos = 0) const noexcept {
        return native_view_type(storage.data(), storage.size()).find(ch, pos);
    }

    bool contains(view_type text) const noexcept {
        return find(text) != npos;
    }

    bool starts_with(view_type text) const noexcept {
        return native_view_type(storage.data(), storage.size())
            .starts_with(typename view_type::view_type(text));
    }

    bool ends_with(view_type text) const noexcept {
        return native_view_type(storage.data(), storage.size())
            .ends_with(typename view_type::view_type(text));
    }

    BasicString substr(std::size_t pos, std::size_t count = npos) const {
        return BasicString(storage.substr(pos, count));
    }

    operator view_type() const noexcept {
        return view_type(storage.data(), storage.size());
    }

    operator native_view_type() const noexcept {
        return native_view_type(storage.data(), storage.size());
    }

    const storage_type& Native() const noexcept {
        return storage;
    }

    storage_type& Native() noexcept {
        return storage;
    }

    bool operator==(view_type other) const noexcept {
        return native_view_type(storage.data(), storage.size()) == typename view_type::view_type(other);
    }

    bool operator==(const CharT* other) const noexcept {
        return native_view_type(storage.data(), storage.size()) == native_view_type(other);
    }

    bool operator==(const BasicString& other) const noexcept {
        return storage == other.storage;
    }

    bool operator!=(const BasicString& other) const noexcept {
        return !(*this == other);
    }

    bool operator<(const BasicString& other) const noexcept {
        return storage < other.storage;
    }

private:
    storage_type storage;
};

using PlatformStringView = BasicStringView<PlatformChar, PlatformEncoding>;
using PlatformString     = BasicString<PlatformChar, PlatformEncoding>;
using StringView         = PlatformStringView;
using String             = PlatformString;

using AsciiStringView = BasicStringView<AsciiChar, AsciiEncoding>;
using AsciiString     = BasicString<AsciiChar, AsciiEncoding>;
using Utf8StringView  = BasicStringView<Char8, Utf8Encoding>;
using Utf8String      = BasicString<Char8, Utf8Encoding>;
using Utf16StringView = BasicStringView<Char16, Utf16Encoding>;
using Utf16String     = BasicString<Char16, Utf16Encoding>;
using Utf32StringView = BasicStringView<Char32, Utf32Encoding>;
using Utf32String     = BasicString<Char32, Utf32Encoding>;
using WideStringView  = BasicStringView<WideChar, WideEncoding>;
using WideString      = BasicString<WideChar, WideEncoding>;

} // namespace Moer

namespace std {

template<typename CharT, typename Encoding, typename Traits>
struct hash<Moer::BasicStringView<CharT, Encoding, Traits>> {
    std::size_t operator()(Moer::BasicStringView<CharT, Encoding, Traits> text) const noexcept {
        return std::hash<std::basic_string_view<CharT, Traits>>{}(text.Native());
    }
};

template<typename CharT, typename Encoding, typename Traits, typename Allocator>
struct hash<Moer::BasicString<CharT, Encoding, Traits, Allocator>> {
    std::size_t operator()(const Moer::BasicString<CharT, Encoding, Traits, Allocator>& text) const noexcept {
        return std::hash<std::basic_string_view<CharT, Traits>>{}(
            std::basic_string_view<CharT, Traits>(text.data(), text.size())
        );
    }
};

} // namespace std
