#ifndef MOERENGINE_CORE_H
#define MOERENGINE_CORE_H
#include "API_Macro.h"
#include "platform/Platform.h"
#include "taskgraph/TaskSystem.h"
#include "config/ConfigMap.h"
#include "misc/EnumBitOperation.h"
#include "misc/StatQueue.h"
#include "misc/Hash.h"
#include "math/Math.h"

namespace Moer {

    CORE_API extern bool IsCurrentlyGameThread();

    CORE_API extern bool IsCurrentlyRenderThread();

    CORE_API extern bool IsGameThreadInitialized();

    CORE_API extern bool IsRenderThreadInitialized();
}// namespace Moer

#endif// !MOER_CORE_H
