#ifndef MOER_DEFERRED_BASE_PASS_H
#define MOER_DEFERRED_BASE_PASS_H
#include "misc/STL.h"
namespace Moer {
    class BasePass {
    public:
        struct Impl;

    private:
        UniquePtr<Impl> impl;
    };
}// namespace Moer

#endif