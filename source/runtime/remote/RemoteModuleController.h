#pragma once

#include "misc/STL.h"
#include "remote/RemoteApi.h"
#include "remote/RemoteConfig.h"

namespace Moer::remote {

// 给 RemoteModuleController 使用的内部桥接接口
class REMOTE_API IRemoteModuleControllerBridge {
public:
    virtual ~IRemoteModuleControllerBridge() = default;

    // 查询 Remote 是否处于启用状态
    virtual bool IsEnabled() const = 0;

    // 查询 Remote 服务是否已经成功启动
    virtual bool IsRunning() const = 0;

    // 请求启用或关闭 Remote 模块
    virtual bool SetEnabled(bool enabled) = 0;

    // 获取当前 Remote 配置快照
    virtual RemoteConfig GetConfigSnapshot() const = 0;
};

// 提供给 Editor 等外部系统持有的轻量控制句柄
// 它本身不拥有 RemoteModule，主要是为了让 EditorUI 只传递一个对象就能控制 Remote 开关
class REMOTE_API RemoteModuleController {
public:
    RemoteModuleController() = default;
    explicit RemoteModuleController(const SharedPtr<IRemoteModuleControllerBridge>& bridge);

    // 判断控制句柄当前是否仍然关联有效的 RemoteModule
    bool IsValid() const;

    // 查询 Remote 是否处于启用状态
    bool IsEnabled() const;

    // 查询 Remote 服务是否已经成功启动
    bool IsRunning() const;

    // 请求启用或关闭 Remote 模块
    bool SetEnabled(bool enabled) const;

    // 获取当前 Remote 配置快照，主要给 Editor 这类外部系统做展示
    RemoteConfig GetConfigSnapshot() const;

private:
    std::weak_ptr<IRemoteModuleControllerBridge> m_bridge;
};

} // namespace Moer::remote