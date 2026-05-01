#pragma once

#include "RenderAPI.h"
#include "scene/cache/SceneCacheHeader.h"

#include <filesystem>
#include <string>

namespace Moer {

// 记录源场景文件的稳定身份，用于命中 cache
struct RENDER_API SceneCacheSourceIdentity {
    std::filesystem::path canonical_path;
    uint64                file_size       = 0;
    int64                 last_write_time = 0;
    uint64                hash            = 0;
    std::string           hash_string;
};

/**
 * Scene Cache 分为两类
 * - Origin Cache: 源场景文件生成的缓存
 * - State Cache: 场景修改后的缓存
 *
 * Scene Cache 文件分工:
 * - SceneCache.h/cpp:           Metadata
 * - SceneCacheHeader.h:         Metadata 布局
 * - SceneCacheSerializer.h/cpp: Data
 *
 * 使用时:
 * - 读缓存: 先 BuildSourceIdentity + GetCachePath + IsCacheValid，再调用 SceneCacheSerializer::LoadLogicalScene
 * - 写缓存: 先 BuildSourceIdentity + CreateHeader，再调用 SceneCacheSerializer::SaveLogicalScene
 */
class RENDER_API SceneCache {
public:
    // 返回 scene cache 的根目录
    static std::filesystem::path GetCacheRoot();

    // 根据源场景文件生成用于命中 cache 的身份信息
    static bool
    BuildSourceIdentity(const std::filesystem::path& source_file, SceneCacheSourceIdentity& out_identity);

    // 根据源文件身份和 cache 类型生成缓存路径
    static std::filesystem::path GetCachePath(const SceneCacheSourceIdentity& identity, ESceneCacheKind kind);

    // 生成一个可直接写入文件头的 header
    static SceneCacheHeader
    CreateHeader(const SceneCacheSourceIdentity& identity, ESceneCacheKind kind, uint64 payload_size = 0);

    // 只写入 header，方便调试 cache 文件
    static bool
    WriteHeaderOnlyForDebug(const std::filesystem::path& cache_path, const SceneCacheHeader& header);

    // 从 cache 文件开头读取 header
    static bool ReadHeader(const std::filesystem::path& cache_path, SceneCacheHeader& out_header);

    // 校验 cache 文件头是否仍与源文件身份匹配
    static bool IsCacheValid(
        const std::filesystem::path&    cache_path,
        const SceneCacheSourceIdentity& identity,
        ESceneCacheKind                 kind,
        SceneCacheHeader*               out_header = nullptr
    );

    // 返回 cache 类型的可读字符串
    static const char* KindToString(ESceneCacheKind kind);

private:
    // 校验 header magic 是否匹配当前格式
    static bool IsMagicValid(const SceneCacheHeader& header);
    // 对源文件身份文本计算稳定哈希值
    static uint64 BuildHashValue(const std::string& source_identity_text);
    // 把哈希值格式化成 cache 文件名字符串
    static std::string BuildHashString(uint64 hash);
};

} // namespace Moer
