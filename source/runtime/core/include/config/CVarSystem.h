#pragma once

#include <charconv>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "API_Macro.h"
#include "misc/STL.h"

#if defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#endif

#if !defined(TEXT)
#define TEXT(x) x
#endif

namespace Moer::CVar {

enum class EType {
    Bool = 0,
    Int,
    Float,
    String,
};

class ICVar {
public:
    ICVar(
        std::string name,
        std::string helper,
        std::string true_helper,
        std::string false_helper,
        EType       type
    ) :
        m_name(std::move(name)),
        m_helper(std::move(helper)),
        m_true_helper(std::move(true_helper)),
        m_false_helper(std::move(false_helper)),
        m_type(type) {}
    virtual ~ICVar() = default;

    std::string_view GetName() const {
        return m_name;
    }
    std::string_view GetHelper() const {
        return m_helper;
    }
    std::string_view GetTrueHelper() const {
        return m_true_helper;
    }
    std::string_view GetFalseHelper() const {
        return m_false_helper;
    }
    EType GetType() const {
        return m_type;
    }

    virtual void        CopyValueString(char* buffer, size_t buffer_size) const = 0;
    virtual const char* SetValueFromString(std::string_view text) = 0;

private:
    std::string m_name;
    std::string m_helper;
    std::string m_true_helper;
    std::string m_false_helper;
    EType       m_type;
};

CORE_API bool        Register(ICVar* cvar);
CORE_API ICVar*      Find(std::string_view name);
using ICVarVisitor = void (*)(ICVar* cvar, void* user_data);
CORE_API void        VisitAll(ICVarVisitor visitor, void* user_data);
CORE_API bool        ApplyIniFile(const std::filesystem::path& file_path);

namespace Detail {

inline std::string ToUtf8Text(const char* text) {
    return text ? std::string(text) : std::string();
}

inline std::string ToUtf8Text(std::string text) {
    return text;
}

inline std::string ToUtf8Text(std::string_view text) {
    return std::string(text);
}

inline std::string ToUtf8Text(const wchar_t* text) {
    if (!text) {
        return {};
    }
#if defined(_WIN32) || defined(_WIN64)
    const int src_len = static_cast<int>(std::wcslen(text));
    if (src_len <= 0) {
        return {};
    }
    const int out_len = WideCharToMultiByte(CP_UTF8, 0, text, src_len, nullptr, 0, nullptr, nullptr);
    if (out_len <= 0) {
        return {};
    }
    std::string out(size_t(out_len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, src_len, out.data(), out_len, nullptr, nullptr);
    return out;
#else
    std::string out;
    while (*text) {
        out.push_back(static_cast<char>(*text));
        ++text;
    }
    return out;
#endif
}

inline std::string ToUtf8Text(std::wstring_view text) {
#if defined(_WIN32) || defined(_WIN64)
    if (text.empty()) {
        return {};
    }
    const int src_len = static_cast<int>(text.size());
    const int out_len = WideCharToMultiByte(CP_UTF8, 0, text.data(), src_len, nullptr, 0, nullptr, nullptr);
    if (out_len <= 0) {
        return {};
    }
    std::string out(size_t(out_len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), src_len, out.data(), out_len, nullptr, nullptr);
    return out;
#else
    std::string out;
    out.reserve(text.size());
    for (wchar_t c : text) {
        out.push_back(static_cast<char>(c));
    }
    return out;
#endif
}

inline std::string ToUtf8Text(const std::wstring& text) {
    return ToUtf8Text(std::wstring_view(text));
}

template<typename T>
struct ValueTraits;

template<>
struct ValueTraits<bool> {
    static constexpr EType type = EType::Bool;
    static std::string     ToString(bool value) {
        return value ? "1" : "0";
    }
    static const char* Parse(std::string_view text, bool& out_value) {
        std::string lower;
        lower.reserve(text.size());
        for (char c : text) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (lower == "1" || lower == "true" || lower == "on") {
            out_value = true;
            return nullptr;
        }
        if (lower == "0" || lower == "false" || lower == "off") {
            out_value = false;
            return nullptr;
        }
        return "Type mismatch: expected bool (0/1, true/false, on/off).";
    }
};

template<>
struct ValueTraits<int> {
    static constexpr EType type = EType::Int;
    static std::string     ToString(int value) {
        return std::to_string(value);
    }
    static const char* Parse(std::string_view text, int& out_value) {
        const std::string t(text);
        if (t.empty()) {
            return "Type mismatch: expected int.";
        }
        const char* begin = t.data();
        const char* end   = t.data() + t.size();
        auto [ptr, ec]    = std::from_chars(begin, end, out_value);
        if (ec != std::errc() || ptr != end) {
            return "Type mismatch: expected int.";
        }
        return nullptr;
    }
};

template<>
struct ValueTraits<float> {
    static constexpr EType type = EType::Float;
    static std::string     ToString(float value) {
        return std::to_string(value);
    }
    static const char* Parse(std::string_view text, float& out_value) {
        const std::string t(text);
        if (t.empty()) {
            return "Type mismatch: expected float.";
        }
        char* parsed_end = nullptr;
        out_value        = std::strtof(t.c_str(), &parsed_end);
        if (parsed_end == nullptr || parsed_end != t.c_str() + t.size() || !std::isfinite(out_value)) {
            return "Type mismatch: expected float.";
        }
        return nullptr;
    }
};

template<>
struct ValueTraits<std::string> {
    static constexpr EType type = EType::String;
    static std::string     ToString(const std::string& value) {
        return value;
    }
    static const char* Parse(std::string_view text, std::string& out_value) {
        out_value = std::string(text);
        return nullptr;
    }
};

inline void CopyTextToBuffer(std::string_view text, char* buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        return;
    }

    const size_t copy_len = (std::min)(text.size(), buffer_size - 1);
    if (copy_len > 0) {
        std::memcpy(buffer, text.data(), copy_len);
    }
    buffer[copy_len] = '\0';
}

template<typename T>
concept SupportedValue =
    std::is_same_v<T, bool> || std::is_same_v<T, int> || std::is_same_v<T, float> ||
    std::is_same_v<T, std::string>;

} // namespace Detail

template<Detail::SupportedValue T>
class TCVar final : public ICVar {
public:
    using OnChangeFunc = std::function<void(const T& old_value, const T& new_value)>;

    template<typename NameT, typename HelperT, typename TrueHelperT, typename FalseHelperT>
    TCVar(
        NameT&&       name,
        const T&      default_value,
        HelperT&&     helper,
        TrueHelperT&& true_helper,
        FalseHelperT&& false_helper,
        OnChangeFunc  on_change = OnChangeFunc{}
    ) :
        ICVar(
            Detail::ToUtf8Text(std::forward<NameT>(name)),
            Detail::ToUtf8Text(std::forward<HelperT>(helper)),
            Detail::ToUtf8Text(std::forward<TrueHelperT>(true_helper)),
            Detail::ToUtf8Text(std::forward<FalseHelperT>(false_helper)),
            Detail::ValueTraits<T>::type
        ),
        m_owned_value(default_value),
        m_value_ptr(&m_owned_value),
        m_on_change(std::move(on_change)) {
        Register(this);
    }

    template<typename NameT, typename HelperT, typename TrueHelperT, typename FalseHelperT>
    TCVar(
        NameT&&        name,
        T&             external_value_ref,
        HelperT&&      helper,
        TrueHelperT&&  true_helper,
        FalseHelperT&& false_helper,
        OnChangeFunc   on_change = OnChangeFunc{}
    ) :
        ICVar(
            Detail::ToUtf8Text(std::forward<NameT>(name)),
            Detail::ToUtf8Text(std::forward<HelperT>(helper)),
            Detail::ToUtf8Text(std::forward<TrueHelperT>(true_helper)),
            Detail::ToUtf8Text(std::forward<FalseHelperT>(false_helper)),
            Detail::ValueTraits<T>::type
        ),
        m_value_ptr(&external_value_ref),
        m_on_change(std::move(on_change)) {
        Register(this);
    }

    const T& Get() const {
        return *m_value_ptr;
    }

    void Set(const T& new_value) {
        const T old_value = *m_value_ptr;
        if constexpr (std::is_same_v<T, float>) {
            if (std::abs(old_value - new_value) <= 1e-6f) {
                return;
            }
        } else {
            if (old_value == new_value) {
                return;
            }
        }
        *m_value_ptr = new_value;
        if (m_on_change) {
            m_on_change(old_value, *m_value_ptr);
        }
    }

    void CopyValueString(char* buffer, size_t buffer_size) const override {
        const std::string value = Detail::ValueTraits<T>::ToString(*m_value_ptr);
        Detail::CopyTextToBuffer(value, buffer, buffer_size);
    }

    const char* SetValueFromString(std::string_view text) override {
        T parsed_value{};
        if (const char* error = Detail::ValueTraits<T>::Parse(text, parsed_value)) {
            return error;
        }
        Set(parsed_value);
        return nullptr;
    }

private:
    T            m_owned_value{};
    T*           m_value_ptr = nullptr;
    OnChangeFunc m_on_change;
};

} // namespace Moer::CVar
