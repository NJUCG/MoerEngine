#pragma once

#include "RenderAPI.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "scene/cache/SceneCacheHeader.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

namespace Moer {

namespace ecs {
class LogicalScene;
}

// 顺序写入 scene cache 二进制数据
class RENDER_API SceneCacheBinaryWriter {
public:
    // 打开一个用于写 cache 的二进制流
    explicit SceneCacheBinaryWriter(const std::filesystem::path& file_path);
    // 关闭 writer 持有的文件流
    ~SceneCacheBinaryWriter();

    SceneCacheBinaryWriter(const SceneCacheBinaryWriter&)            = delete;
    SceneCacheBinaryWriter& operator=(const SceneCacheBinaryWriter&) = delete;

    // 返回 writer 当前是否可继续写入
    bool IsValid() const;
    // 返回 writer 记录的首个错误
    const std::string& GetError() const;

    // 写入一个 POD 值
    template<typename T>
        requires std::is_trivially_copyable_v<T>
    bool WritePod(const T& value) {
        return WriteBytes(&value, sizeof(T));
    }

    // 写入一段原始字节
    bool WriteBytes(const void* data, uint64 size);
    // 以长度前缀写入字符串
    bool WriteString(std::string_view value);
    // 移动写指针到指定偏移
    bool Seek(uint64 position);
    // 刷新底层输出流
    bool Flush();
    // 返回当前写指针位置
    uint64 Tell();

    // 以长度前缀写入一个 POD 数组
    template<typename T>
        requires std::is_trivially_copyable_v<T>
    bool WriteArray(const Array<T>& values) {
        const uint64 count = static_cast<uint64>(values.size());
        if (!WritePod(count)) {
            return false;
        }
        if (values.empty()) {
            return true;
        }
        return WriteBytes(values.data(), static_cast<uint64>(values.size() * sizeof(T)));
    }

private:
    // 记录首个 writer 错误
    void RecordError(std::string message);

private:
    std::ofstream m_stream;
    std::string   m_error;
};

// 顺序读取 scene cache 二进制数据
class RENDER_API SceneCacheBinaryReader {
public:
    // 打开一个用于读 cache 的二进制流
    explicit SceneCacheBinaryReader(const std::filesystem::path& file_path);
    // 关闭 reader 持有的文件流
    ~SceneCacheBinaryReader();

    SceneCacheBinaryReader(const SceneCacheBinaryReader&)            = delete;
    SceneCacheBinaryReader& operator=(const SceneCacheBinaryReader&) = delete;

    // 返回 reader 当前是否可继续读取
    bool IsValid() const;
    // 返回 reader 记录的首个错误
    const std::string& GetError() const;

    // 读取一个 POD 值
    template<typename T>
        requires std::is_trivially_copyable_v<T>
    bool ReadPod(T& out_value) {
        return ReadBytes(&out_value, sizeof(T));
    }

    // 读取一段原始字节
    bool ReadBytes(void* out_data, uint64 size);
    // 读取一个长度前缀字符串
    bool ReadString(std::string& out_value);

    // 读取一个带长度前缀的 POD 数组
    template<typename T>
        requires std::is_trivially_copyable_v<T>
    bool ReadArray(Array<T>& out_values) {
        uint64 count = 0;
        if (!ReadPod(count)) {
            return false;
        }
        if (count > static_cast<uint64>(std::numeric_limits<size_t>::max())) {
            RecordError("Scene cache array is too large for this platform.");
            return false;
        }

        out_values.resize(static_cast<size_t>(count));
        if (out_values.empty()) {
            return true;
        }
        return ReadBytes(out_values.data(), static_cast<uint64>(out_values.size() * sizeof(T)));
    }

private:
    // 记录首个 reader 错误
    void RecordError(std::string message);

private:
    std::ifstream m_stream;
    std::string   m_error;
};

// 负责 LogicalScene 与 cache payload 的互相转换
class RENDER_API SceneCacheSerializer {
public:
    // 把 LogicalScene 序列化为 cache 文件
    static bool SaveLogicalScene(
        const std::filesystem::path& cache_path,
        SceneCacheHeader             header,
        const ecs::LogicalScene&     logical_scene
    );

    // 从 cache 文件反序列化一个 LogicalScene
    static bool LoadLogicalScene(
        const std::filesystem::path& cache_path,
        ecs::LogicalScene&           logical_scene,
        SceneCacheHeader*            out_header = nullptr
    );
};

} // namespace Moer
