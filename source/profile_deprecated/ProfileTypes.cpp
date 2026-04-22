#include "ProfileTypes.h"

#include "../runtime/core/include/profile/ProfileDump.h"
#include "../runtime/core/include/profile/ProfileDumpTemplates.h"

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

    const uint64_t thread_id = static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const uint32_t depth = static_cast<uint32_t>(stack.size());
    DUMP_STREAM(Moer::ProfileDump::Templates::CpuScopeTemplate)
        << thread_id
        << name
        << static_cast<int64_t>(start)
        << static_cast<int64_t>(now - start)
        << depth;
}

void FlameProfiler::Save(const std::string& path) {
    Moer::ProfileDump::OverrideOutputFileForCurrentSession(path);
    Moer::ProfileDump::FlushThreadLocal();
    Moer::ProfileDump::FlushAll();
}

const char* GetSourceStr(MemorySource s) {
    static const char* sources[] = { "Editor", "Vulkan", "VulkanTmp" };
    return sources[(int)s];
}

const char* GetActionStr(MemoryAction a) {
    return (a == MemoryAction::Alloc) ? "ALLOC" : "FREE";
}

const char* GetVkUsageName(uint32_t usage) {
    static const char* labels[] = {
        "Upload",
        "Readback",
        "Scratch",
        "ShaderBuffer",
        "ShaderBuffer_Constant"
    };

    if (usage < (uint32_t)EVkInternalBufferUsage::Count) {
        return labels[usage];
    }
    return "Unknown/ByName";
}