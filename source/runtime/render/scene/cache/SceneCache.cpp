#include "scene/cache/SceneCache.h"

#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "misc/Hash.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <system_error>

namespace Moer {

namespace {
constexpr char k_scene_cache_magic[16] =
    {'M', 'O', 'E', 'R', '_', 'S', 'C', 'E', 'N', 'E', '_', 'C', 'A', 'C', 'H', 'E'};
constexpr uint32 k_scene_cache_format_version = 1;

// 尽量把路径归一化为稳定的绝对路径
std::filesystem::path CanonicalizeExistingPath(const std::filesystem::path& path) {
    std::error_code       ec;
    std::filesystem::path canonical_path = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical_path;
    }
    return std::filesystem::absolute(path);
}

// 统一记录一次 cache miss
bool LogCacheMiss(ESceneCacheKind kind, const std::filesystem::path& cache_path, std::string_view reason) {
    LOG_INFO(
        "Scene {} Cache miss: path={}, reason={}", SceneCache::KindToString(kind), cache_path.string(), reason
    );
    return false;
}

} // namespace

// 返回 scene cache 的根目录
std::filesystem::path SceneCache::GetCacheRoot() {
    const std::filesystem::path& resource_root = ConfigManager::GetInstance().GetEditorResourcePath();
    if (!resource_root.empty()) {
        return resource_root / "scene_cache";
    }
    return std::filesystem::path("asset") / "scene_cache";
}

// 根据源文件生成用于命中 cache 的身份信息
bool SceneCache::BuildSourceIdentity(
    const std::filesystem::path& source_file,
    SceneCacheSourceIdentity&    out_identity
) {
    std::error_code ec;
    const bool      exists = std::filesystem::exists(source_file, ec);
    if (ec) {
        LOG_WARNING(
            "Scene Cache source identity build failed: source={}, step=exists, filesystem_error={}",
            source_file.string(),
            ec.message()
        );
        return false;
    }
    if (!exists) {
        LOG_WARNING(
            "Scene Cache source identity build failed: source={}, reason=source file does not exist",
            source_file.string()
        );
        return false;
    }

    const bool is_regular_file = std::filesystem::is_regular_file(source_file, ec);
    if (ec) {
        LOG_WARNING(
            "Scene Cache source identity build failed: source={}, step=is_regular_file, filesystem_error={}",
            source_file.string(),
            ec.message()
        );
        return false;
    }
    if (!is_regular_file) {
        LOG_WARNING(
            "Scene Cache source identity build failed: source={}, reason=source path is not a regular file",
            source_file.string()
        );
        return false;
    }

    const std::filesystem::path canonical_path = CanonicalizeExistingPath(source_file);
    const uint64 file_size = static_cast<uint64>(std::filesystem::file_size(canonical_path, ec));
    if (ec) {
        LOG_WARNING(
            "Scene Cache source identity build failed: source={}, step=file_size, filesystem_error={}",
            canonical_path.string(),
            ec.message()
        );
        return false;
    }

    const auto last_write_time = std::filesystem::last_write_time(canonical_path, ec);
    if (ec) {
        LOG_WARNING(
            "Scene Cache source identity build failed: source={}, step=last_write_time, filesystem_error={}",
            canonical_path.string(),
            ec.message()
        );
        return false;
    }

    const int64 last_write_time_count = static_cast<int64>(last_write_time.time_since_epoch().count());

    std::ostringstream identity_stream;
    identity_stream << canonical_path.generic_string() << '|' << file_size << '|' << last_write_time_count;
    const std::string identity_text = identity_stream.str();
    const uint64      hash          = BuildHashValue(identity_text);

    out_identity.canonical_path  = canonical_path;
    out_identity.file_size       = file_size;
    out_identity.last_write_time = last_write_time_count;
    out_identity.hash            = hash;
    out_identity.hash_string     = BuildHashString(hash);
    return true;
}

// 根据源身份和类型拼出 cache 文件路径
std::filesystem::path
SceneCache::GetCachePath(const SceneCacheSourceIdentity& identity, ESceneCacheKind kind) {
    const char* extension = kind == ESceneCacheKind::Origin ? ".origin.moca" : ".moca";
    return GetCacheRoot() / (identity.hash_string + extension);
}

// 构造一个写入前使用的 cache 文件头
SceneCacheHeader SceneCache::CreateHeader(
    const SceneCacheSourceIdentity& identity,
    ESceneCacheKind                 kind,
    uint64                          payload_size
) {
    SceneCacheHeader header{};
    header.format_version              = k_scene_cache_format_version;
    header.header_size                 = sizeof(SceneCacheHeader);
    header.kind                        = static_cast<uint32>(kind);
    header.source_identity_hash        = identity.hash;
    header.source_file_size            = identity.file_size;
    header.source_file_last_write_time = identity.last_write_time;
    header.payload_size                = payload_size;
    return header;
}

// 只写文件头，方便调试 cache 格式
bool SceneCache::WriteHeaderOnlyForDebug(
    const std::filesystem::path& cache_path,
    const SceneCacheHeader&      header
) {
    std::error_code ec;
    std::filesystem::create_directories(cache_path.parent_path(), ec);
    if (ec) {
        LOG_WARNING(
            "Scene Cache debug header write failed: path={}, step=create_directories, filesystem_error={}",
            cache_path.string(),
            ec.message()
        );
        return false;
    }

    std::ofstream stream(cache_path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        LOG_WARNING("Scene Cache debug header write failed: path={}, step=open", cache_path.string());
        return false;
    }

    stream.write(reinterpret_cast<const char*>(&header), sizeof(SceneCacheHeader));
    if (!stream.good()) {
        LOG_WARNING("Scene Cache debug header write failed: path={}, step=write_header", cache_path.string());
        return false;
    }
    return true;
}

// 从 cache 文件开头读取文件头
bool SceneCache::ReadHeader(const std::filesystem::path& cache_path, SceneCacheHeader& out_header) {
    std::ifstream stream(cache_path, std::ios::binary);
    if (!stream.is_open()) {
        LOG_WARNING("Scene Cache header read failed: path={}, step=open", cache_path.string());
        return false;
    }

    stream.read(reinterpret_cast<char*>(&out_header), sizeof(SceneCacheHeader));
    if (stream.gcount() != static_cast<std::streamsize>(sizeof(SceneCacheHeader))) {
        LOG_WARNING(
            "Scene Cache header read failed: path={}, reason=file too small to contain a header",
            cache_path.string()
        );
        return false;
    }
    return true;
}

// 校验 cache 文件头是否仍与源文件一致
bool SceneCache::IsCacheValid(
    const std::filesystem::path&    cache_path,
    const SceneCacheSourceIdentity& identity,
    ESceneCacheKind                 kind,
    SceneCacheHeader*               out_header
) {
    std::error_code ec;
    const bool      exists = std::filesystem::exists(cache_path, ec);
    if (ec) {
        LOG_WARNING(
            "Scene {} Cache validation failed: path={}, step=exists, filesystem_error={}",
            KindToString(kind),
            cache_path.string(),
            ec.message()
        );
        return false;
    }
    if (!exists) {
        return LogCacheMiss(kind, cache_path, "cache file does not exist");
    }

    SceneCacheHeader header{};
    if (!ReadHeader(cache_path, header)) {
        return false;
    }

    if (!IsMagicValid(header)) {
        return LogCacheMiss(kind, cache_path, "cache magic mismatch");
    }
    if (header.format_version != k_scene_cache_format_version) {
        return LogCacheMiss(kind, cache_path, "cache format version mismatch");
    }
    if (header.header_size != sizeof(SceneCacheHeader)) {
        return LogCacheMiss(kind, cache_path, "cache header size mismatch");
    }
    if (header.kind != static_cast<uint32>(kind)) {
        return LogCacheMiss(kind, cache_path, "cache kind mismatch");
    }
    if (header.source_identity_hash != identity.hash) {
        return LogCacheMiss(kind, cache_path, "source identity hash mismatch");
    }
    if (header.source_file_size != identity.file_size) {
        return LogCacheMiss(kind, cache_path, "source file size mismatch");
    }
    if (header.source_file_last_write_time != identity.last_write_time) {
        return LogCacheMiss(kind, cache_path, "source file last write time mismatch");
    }

    if (out_header) {
        *out_header = header;
    }
    return true;
}

// 返回 cache 类型的可读字符串
const char* SceneCache::KindToString(ESceneCacheKind kind) {
    switch (kind) {
        case ESceneCacheKind::Origin:
            return "Origin";
        case ESceneCacheKind::State:
            return "State";
    }
    return "Unknown";
}

// 判断 header magic 是否匹配当前格式
bool SceneCache::IsMagicValid(const SceneCacheHeader& header) {
    return std::memcmp(header.magic, k_scene_cache_magic, sizeof(k_scene_cache_magic)) == 0;
}

// 对源身份文本计算稳定哈希值
uint64 SceneCache::BuildHashValue(const std::string& source_identity_text) {
    Hash64City       hasher;
    std::string_view source_identity_view(source_identity_text);
    hasher.FromString(source_identity_view);

    uint64 hash = 0;
    std::memcpy(&hash, hasher.hash_code.data(), sizeof(hash));
    return hash;
}

// 把哈希值格式化成文件名片段
std::string SceneCache::BuildHashString(uint64 hash) {
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

} // namespace Moer
