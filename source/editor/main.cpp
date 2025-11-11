#include "Editor.h"
#include <iostream>

int main(int argc, const char** argv) {
    std::cout << "Moer Engine Editor Starting..." << std::endl;

    Moer::Editor editor;

    editor.Init(argc, argv);

    editor.Run();

    editor.ShutDown();

    return 0;
}