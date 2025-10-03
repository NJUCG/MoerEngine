#include "window/WindowInput.h"

#include "misc/STL.h"

namespace Moer {

    WindowInput& WindowInput::Get() {
        static WindowInput wndInput;
        return wndInput;
    }

}// namespace Moer