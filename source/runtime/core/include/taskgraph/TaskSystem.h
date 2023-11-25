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
        static void ShutDown();
    };
}// namespace Moer

#endif// !TASK_SYSTEM_H
