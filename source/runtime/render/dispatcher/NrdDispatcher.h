#pragma once

#include "NRD.h"

#include "RenderAPI.h"
#include "misc/STL.h"

namespace Moer::Render::Nrd {
class NrdDispatcher {
public:
    struct Impl;

    RENDER_API NrdDispatcher();
    RENDER_API virtual ~NrdDispatcher();

private:
    UniquePtr<Impl> impl;
};
} // namespace Moer::Render::Nrd
