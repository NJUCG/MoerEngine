#include "ProfileTypes.h"
#include "misc/Types.h"

void FlameProfiler::Begin(const char* name) {
    GetStack().push_back({name, NowUs()});
}

void FlameProfiler::End() {
    auto now = NowUs();
    auto& stack = GetStack();
    if (stack.empty())
    {
        return;
    } 
    auto [name, start] = stack.back();
    stack.pop_back();

    FlameEvent e;
    e.name = name;
    e.start_us = start;
    e.duration_us = now - start;
    e.thread_id   = (Moer::uint32)std::hash<std::thread::id>{}(std::this_thread::get_id());

    std::lock_guard<std::mutex> lock(m_mutex);
    m_events.push_back(e);
    //printf("[profiler] push_back m_events:%d\n", m_events.size());
}

void FlameProfiler::Save(const std::string& path) {
    //std::lock_guard<std::mutex> lock(m_mutex);
    std::ofstream f(path);
    f << "{\"traceEvents\":[\n";
    printf("[profiler] m_events:%zd\n", m_events.size());
    for (size_t i = 0; i < m_events.size(); i++) {
        auto& e = m_events[i];
        f << "{\"name\":\"" << e.name << "\","
            << "\"ph\":\"X\","
            << "\"ts\":" << e.start_us << ","
            << "\"dur\":" << e.duration_us << ","
            << "\"pid\":0,"
            << "\"tid\":" << e.thread_id << "}";
        if (i + 1 < m_events.size()) f << ",";
        f << "\n";
    }
    f << "]}\n";
}

const char* GetSourceStr(MemorySource s) {
    static const char* sources[] = { "Editor", "Vulkan", "VulkanTmp" };
    return sources[(int)s];
}

const char* GetActionStr(MemoryAction a) {
    return (a == MemoryAction::Alloc) ? "ALLOC" : "FREE";
}

const char* GetVkUsageName(Moer::uint32 usage) {
    static const char* labels[] = {
        "Upload",
        "Readback",
        "Scratch",
        "ShaderBuffer",
        "ShaderBuffer_Constant"
    };

    if (usage < (Moer::uint32)EVkInternalBufferUsage::Count) {
        return labels[usage];
    }
    return "Unknown/ByName";
}