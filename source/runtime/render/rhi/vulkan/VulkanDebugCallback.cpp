
#include "VulkanDebugCallback.h"

#include "log/LogSystem.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Moer::Render {

// move aggregator types/state to file scope so both DebugCallback and flusher can access
namespace {

struct BufEntry {
    std::string                           text;
    std::chrono::steady_clock::time_point ts;
    int                                   max_sev; // 0=verbose,1=info,2=warn,3=error
};
static std::mutex                                s_debugbuf_mutex;
static std::unordered_map<std::string, BufEntry> s_debugbuf;
static std::atomic_bool                          s_logged_unknown_coop_matrix_layer{false};
static std::atomic_bool                          s_logged_unknown_coop_vector_layer{false};
static std::atomic_bool                          s_suppress_device_loader_callstack{false};

void LogUnknownCooperativeLayerInfo(std::atomic_bool& _once_flag, const char* _extension_name) {
    bool expected = false;
    if (_once_flag.compare_exchange_strong(expected, true)) {
        LOG_INFO(
            "VulkanRHI: Validation layer does not recognize {}. Consider updating VulkanSDK to 1.4 or newer "
            "for accurate validation results.",
            _extension_name
        );
    }
}

bool TryHandleKnownCooperativeLayerWarning(const VkDebugUtilsMessengerCallbackDataEXT* _callback_data) {
    if (_callback_data == nullptr || _callback_data->pMessage == nullptr) {
        return false;
    }

    const std::string_view message(_callback_data->pMessage);
    const std::string_view message_id_name(
        _callback_data->pMessageIdName != nullptr ? _callback_data->pMessageIdName : ""
    );
    if (message.find("is not supported by this layer") == std::string_view::npos) {
        const bool is_unknown_pnext_struct =
            message.find("unknown VkStructureType") != std::string_view::npos;
        if (is_unknown_pnext_struct && message_id_name == "VUID-VkDeviceCreateInfo-pNext-pNext" &&
            message.find("vkCreateDevice()") != std::string_view::npos &&
            message.find("VkStructureType (1000491000)") != std::string_view::npos) {
            LogUnknownCooperativeLayerInfo(
                s_logged_unknown_coop_vector_layer, VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME
            );
            return true;
        }

        if (is_unknown_pnext_struct && message_id_name == "VUID-VkPhysicalDeviceProperties2-pNext-pNext" &&
            message.find("vkGetPhysicalDeviceProperties2()") != std::string_view::npos &&
            message.find("VkStructureType (1000491001)") != std::string_view::npos) {
            LogUnknownCooperativeLayerInfo(
                s_logged_unknown_coop_vector_layer, VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME
            );
            return true;
        }

        return false;
    }

    if (message.find(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME) != std::string_view::npos) {
        LogUnknownCooperativeLayerInfo(
            s_logged_unknown_coop_matrix_layer, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME
        );
        return true;
    }

    if (message.find(VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME) != std::string_view::npos) {
        LogUnknownCooperativeLayerInfo(
            s_logged_unknown_coop_vector_layer, VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME
        );
        return true;
    }

    return false;
}

bool TryHandleRedundantLoaderMessage(const VkDebugUtilsMessengerCallbackDataEXT* _callback_data) {
    if (_callback_data == nullptr || _callback_data->pMessage == nullptr) {
        return false;
    }

    const std::string_view message(_callback_data->pMessage);
    const std::string_view message_id_name(
        _callback_data->pMessageIdName != nullptr ? _callback_data->pMessageIdName : ""
    );
    if (message_id_name != "Loader Message") {
        return false;
    }

    if (message.find("Inserted device layer \"") != std::string_view::npos) {
        return true;
    }

    if (message.find("vkCreateDevice layer callstack setup to:") != std::string_view::npos) {
        s_suppress_device_loader_callstack.store(true);
        return true;
    }

    if (!s_suppress_device_loader_callstack.load()) {
        return false;
    }

    if (message.find("Using \"") != std::string_view::npos &&
        message.find("\" with driver: \"") != std::string_view::npos) {
        s_suppress_device_loader_callstack.store(false);
        return false;
    }

    return true;
}

} // namespace

static void OutputMessage(int sev, const std::string& msg) {
    if (sev >= 3) {
        LOG_ERROR("\n{}", msg);
    } else if (sev == 2) {
        LOG_WARNING("\n{}", msg);
    } else if (sev == 1) {
        LOG_INFO("\n{}", msg);
    } else {
        LOG_DEBUG("\n{}", msg);
    }
}

// flush function to be called periodically (e.g., end of frame) or at program exit
void FlushBufferedDebugMessages() {
    std::unordered_map<std::string, BufEntry> to_flush;
    {
        std::lock_guard<std::mutex> lg(s_debugbuf_mutex);
        to_flush.swap(s_debugbuf);
    }
    for (auto& kv : to_flush) {
        const auto& entry = kv.second;
        OutputMessage(entry.max_sev, entry.text);
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      message_severity,
    VkDebugUtilsMessageTypeFlagsEXT             message_type,
    const VkDebugUtilsMessengerCallbackDataEXT* p_callback_data,
    void*                                       p_user_data
) {
    if (p_callback_data == nullptr)
        return VK_FALSE;

    if (TryHandleKnownCooperativeLayerWarning(p_callback_data)) {
        return VK_FALSE;
    }

    if (TryHandleRedundantLoaderMessage(p_callback_data)) {
        return VK_FALSE;
    }

    // preserve original formatting: normalize CRLF -> LF, trim leading/trailing whitespace/newlines
    auto flatten_message = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        // normalize CR (CRLF -> LF) by skipping '\r'
        for (unsigned char c : s) {
            if (c == '\r')
                continue;
            out.push_back(static_cast<char>(c));
        }
        // trim leading whitespace/newlines
        size_t start = 0;
        while (start < out.size() && (out[start] == ' ' || out[start] == '\t' || out[start] == '\n'))
            ++start;
        size_t end = out.size();
        while (end > start && (out[end - 1] == ' ' || out[end - 1] == '\t' || out[end - 1] == '\n'))
            --end;
        return out.substr(start, end - start);
    };

    std::stringstream ss;
    ss << "[" << p_callback_data->messageIdNumber << "] "
       << "[" << (p_callback_data->pMessageIdName ? p_callback_data->pMessageIdName : "")
       << "]: " << (p_callback_data->pMessage ? p_callback_data->pMessage : "");
    std::string part = flatten_message(ss.str());

    // severity mapping: 0=verbose,1=info,2=warn,3=error
    int sev_val = 0;
    if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        sev_val = 3;
    else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        sev_val = 2;
    else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
        sev_val = 1;
    else
        sev_val = 0;

    const auto     now          = std::chrono::steady_clock::now();
    constexpr auto merge_window = std::chrono::milliseconds(8);

    std::string key = std::to_string(p_callback_data->messageIdNumber) + ":" +
                      (p_callback_data->pMessageIdName ? p_callback_data->pMessageIdName : "");

    std::string to_log; // merged text to flush immediately (if any)
    int         to_log_sev = sev_val;

    {
        std::lock_guard<std::mutex> lg(s_debugbuf_mutex);
        auto                        it = s_debugbuf.find(key);
        if (it == s_debugbuf.end()) {
            // new entry -> store part
            s_debugbuf.emplace(key, BufEntry{part, now, sev_val});
        } else {
            // existing entry
            if (now - it->second.ts <= merge_window) {
                // within merge window: append (with newline) preserving internal newlines
                if (!it->second.text.empty() && it->second.text.back() != '\n')
                    it->second.text.push_back('\n');
                it->second.text.append(part);
                it->second.ts      = now;
                it->second.max_sev = std::max(it->second.max_sev, sev_val);
            } else {
                // older entry: flush existing now, replace with current part
                to_log     = std::move(it->second.text);
                to_log_sev = it->second.max_sev;
                // replace buffer entry with current part
                it->second.text    = part;
                it->second.ts      = now;
                it->second.max_sev = sev_val;
            }
        }

        // If current severity is error, prefer immediate flush to avoid losing urgent info.
        // For errors we flush both any previous (to_log) and the current buffered part right away,
        // and remove the key from buffer to avoid duplicates.
        if (sev_val >= 3) {
            // collect existing buffered text (if the new part was just stored, capture it)
            auto it2 = s_debugbuf.find(key);
            if (it2 != s_debugbuf.end()) {
                // merge previously scheduled flush (to_log) with current buffer content
                if (!to_log.empty()) {
                    // ensure newline separation if needed
                    if (!to_log.empty() && !it2->second.text.empty() && to_log.back() != '\n')
                        to_log.push_back('\n');
                    to_log.append(it2->second.text);
                } else {
                    to_log     = it2->second.text;
                    to_log_sev = it2->second.max_sev;
                }
                // erase from buffer to avoid later duplicate flush
                s_debugbuf.erase(it2);
            } else {
                // nothing in buffer other than current -> flush current immediately
                to_log     = part;
                to_log_sev = sev_val;
            }
        }

        // Safety: avoid unbounded buffering. If a buffer entry grows very large, flush it immediately.
        if (s_debugbuf.size() > 1024) {
            // move all into to_log as best-effort; clear buffer
            std::string combined;
            int         combined_sev = 0;
            for (auto& kv : s_debugbuf) {
                if (!combined.empty())
                    combined.append("\n");
                combined.append(kv.second.text);
                combined_sev = std::max(combined_sev, kv.second.max_sev);
            }
            s_debugbuf.clear();
            to_log     = std::move(combined);
            to_log_sev = combined_sev;
        }
    } // unlock

    // perform actual logging of merged content outside lock
    if (!to_log.empty()) {
        OutputMessage(to_log_sev, to_log);
    }

    // Return VK_FALSE to tell Vulkan to continue as usual. Actual buffered entries will be flushed
    // when FlushBufferedDebugMessages() is called (e.g., once per frame) or by severity-triggered immediate flush above.
    return VK_FALSE;
}

} // namespace Moer::Render
