#ifndef TASK_SYSTEM_H
#define TASK_SYSTEM_H

#include "TaskGraph.h"
namespace Moer {
CORE_API bool  TaskGraphTest();
class CORE_API TaskSystem {
public:
    TaskSystem() = default;
    ~TaskSystem() {}
    static void Init();
    // External and named-thread producers must be quiescent before shutdown,
    // and every named-thread owner must already be stopped and externally
    // joined before TaskGraph releases its named queues and pooled Events.
    // Already-running AnyThread tasks may still publish AnyThread
    // continuations; they are drained before workers close. Named-target work
    // must already be complete and cannot be published during drain. Any late
    // external or named-target publication is a fatal lifecycle-contract
    // violation because QueueTask cannot safely return task ownership.
    static void ShutDown();
};
} // namespace Moer

#endif // !TASK_SYSTEM_H
