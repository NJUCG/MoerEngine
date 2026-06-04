#pragma once

#include "scripting/ScriptingApi.h"

#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Moer {
class Scene;
}

namespace Moer::scripting {

// 管理 scripting thread 提交到主线程执行的 Scene 命令
class SCRIPTING_API MainThreadCommandQueue {
public:
    MainThreadCommandQueue() = default;
    ~MainThreadCommandQueue();

    MainThreadCommandQueue(const MainThreadCommandQueue&)            = delete;
    MainThreadCommandQueue& operator=(const MainThreadCommandQueue&) = delete;

    template<typename Fn>
    auto Submit(Fn&& fn) -> std::future<std::invoke_result_t<std::decay_t<Fn>&, Scene&>> {
        using StoredFn   = std::decay_t<Fn>;
        using ResultType = std::invoke_result_t<StoredFn&, Scene&>;

        auto command = std::make_unique<PendingCommand<StoredFn>>(std::forward<Fn>(fn));
        auto future  = command->GetFuture();
        Enqueue(std::move(command));
        return future;
    }

    void ProcessPendingCommands(Scene& scene);
    void CancelPending(std::string_view reason);

private:
    struct PendingCommandBase {
        virtual ~PendingCommandBase()                = default;
        virtual void Execute(Scene& scene)           = 0;
        virtual void Cancel(std::string_view reason) = 0;
    };

    template<typename Fn>
    struct PendingCommand final : PendingCommandBase {
        using ResultType = std::invoke_result_t<Fn&, Scene&>;

        explicit PendingCommand(Fn&& fn) : m_fn(std::move(fn)) {}

        std::future<ResultType> GetFuture() {
            return m_promise.get_future();
        }

        void Execute(Scene& scene) override {
            try {
                if constexpr (std::is_void_v<ResultType>) {
                    std::invoke(m_fn, scene);
                    m_promise.set_value();
                } else {
                    m_promise.set_value(std::invoke(m_fn, scene));
                }
            } catch (...) {
                m_promise.set_exception(std::current_exception());
            }
        }

        void Cancel(std::string_view reason) override {
            m_promise.set_exception(std::make_exception_ptr(std::runtime_error(std::string(reason))));
        }

        Fn                       m_fn;
        std::promise<ResultType> m_promise;
    };

    void Enqueue(std::unique_ptr<PendingCommandBase> command);

    std::mutex                                      m_mutex;
    std::deque<std::unique_ptr<PendingCommandBase>> m_pending_commands;
};

} // namespace Moer::scripting