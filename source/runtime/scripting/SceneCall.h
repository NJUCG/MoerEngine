#pragma once

#include "scripting/MainThreadCommandQueue.h"

#include <type_traits>
#include <utility>

namespace Moer {
class Scene;
}

namespace Moer::scripting {

template<typename Fn>
auto CallScene(MainThreadCommandQueue& command_queue, Fn&& fn)
    -> std::invoke_result_t<std::decay_t<Fn>&, Scene&> {
    auto future = command_queue.Submit(std::forward<Fn>(fn));
    return future.get();
}

} // namespace Moer::scripting