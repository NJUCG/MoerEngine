#include "Editor.h"

int main(int argc, const char** argv) {

    mi_version();
    Moer::Editor editor;

    editor.Init(argc, argv);

    editor.Run();

    editor.ShutDown();

    return 0;
}