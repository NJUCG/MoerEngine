#ifndef MOERENGINE_CORE_H
#define MOERENGINE_CORE_H
#include "API_Macro.h"
#include "math/Math.h"
#include "misc/Crc32.h"
#include "misc/EnumBitOperation.h"
#include "misc/Hash.h"
#include "misc/LockFree.h"
#include "misc/MMemory.h"
#include "misc/RAII.h"
#include "misc/STL.h"
#include "misc/Singleton.h"
#include "misc/Traits.h"
#include "platform/Platform.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskSystem.h"
namespace Moer {

CORE_API extern bool IsCurrentlyGameThread();

CORE_API extern bool IsCurrentlyRenderThread();

CORE_API extern bool IsGameThreadInitialized();

CORE_API extern bool IsRenderThreadInitialized();

CORE_API extern uint32_t GetRenderThreadId();

CORE_API extern uint32_t GetGameThreadId();
} // namespace Moer

#endif // !MOER_CORE_H
