#pragma once

#include "Core.h"

namespace Moer {

class RuntimeProfiler {
public:
    void TickUI();

    bool IsOpen() const {
        return m_open;
    }

    void SetOpen(bool open) {
        m_open = open;
    }

private:
    void DrawPassAndChildren(const char* parent_name, int depth);

private:
    bool m_open = false;
};

} // namespace Moer