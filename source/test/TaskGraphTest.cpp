#include "Core.h"

#ifdef _WIN32
#include <Windows.h>
static void msleep(unsigned long msecs) { Sleep(msecs); }
#else
#include <unistd.h>
static void msleep(unsigned long msecs) { usleep(msecs * 1000UL); }
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <mimalloc-new-delete.h>
#endif

int main() {
    Moer::TaskSystem::Init();
    Moer::TaskGraphTest();

    return 0;
}