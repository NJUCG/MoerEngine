#pragma once

#include "remote/RemoteApi.h"
#include "remote/RemoteEvent.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace Moer::remote {

// 定义一类接收 Remote 事件的回调函数签名
using RemoteEventCallback = std::function<void(const RemoteEvent&)>;

// 提供线程安全的内存事件发布订阅能力
class REMOTE_API RemoteEventHub {
public:
    // 注册一个事件订阅回调并返回订阅 ID
    uint64_t Subscribe(RemoteEventCallback callback);

    // 根据订阅 ID 取消对应的事件订阅
    void Unsubscribe(uint64_t subscription_id);

    // 向所有当前订阅者广播一条事件
    void Publish(RemoteEvent event);

private:
    std::mutex                                        m_mutex;
    std::unordered_map<uint64_t, RemoteEventCallback> m_callbacks;
    uint64_t                                          m_next_subscription_id = 1;
};

} // namespace Moer::remote