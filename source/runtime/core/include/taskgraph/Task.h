#ifndef TASK_H
#define TASK_H
#include "TaskGraph.h"
#include <atomic>
class TaskHandle {
public:
    TaskHandle() = default;
    ~TaskHandle() {}
};
class TTask : TaskHandle {
public:
    TTask() = default;
    virtual ~TTask() {}
};
#endif // !TASK_H
