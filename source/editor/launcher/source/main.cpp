#include "../include/Launcher.h"

int main(int argc, char** argv) {
#if VULKAN
    return 1;
#endif
    return 0;
}