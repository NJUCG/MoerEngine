// Validates editor command-line arguments and owns the top-level Editor lifecycle.

#include "Editor.h"
#include "Engine.h"

#include <iostream>

int main(int argc, const char** argv) {
    std::cout << "Moer Engine Editor Starting..." << std::endl;

    try {
        Moer::Engine::ValidateCommandLine(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "Invalid command line: " << error.what() << std::endl;
        return 2;
    }

    Moer::Editor editor;

    editor.Init(argc, argv);

    editor.Run();

    editor.ShutDown();

    return 0;
}
