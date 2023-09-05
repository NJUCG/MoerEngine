#ifndef TASK_SYSTEM_H
#define TASK_SYSTEM_H

#include "TaskGraph.h"
namespace __ENGINE_NAME__ {
	class TaskSystem {
	public:
		TaskSystem() = default;
		~TaskSystem() {}
		static void init();
		static void shutDown();

	};
}

#endif // !TASK_SYSTEM_H
