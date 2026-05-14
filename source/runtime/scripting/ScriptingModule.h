#pragma once

#include "scripting/ScriptingApi.h"

namespace Moer::scripting {

class MainThreadCommandQueue;

SCRIPTING_API void SetActiveSceneCommandQueue(MainThreadCommandQueue* command_queue);
SCRIPTING_API void ClearActiveSceneCommandQueue();

} // namespace Moer::scripting