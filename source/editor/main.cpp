#include "Editor.h"
#include <iostream>

#ifdef WITH_PROFILE
#include "profile.h"
#endif

int main(int argc, const char** argv) {
#ifdef WITH_PROFILE
    std::cout << "Moer Engine Perfetto Starting..." << std::endl;
    InitPerfetto();
    std::unique_ptr<perfetto::TracingSession> g_session = StartSession();
    //TRACE_FUNC("Rendering");
    initialize_mimalloc_profiler();
    //std::thread test_thread(memory_test_thread);
#else
    std::cout << "Moer Engine Perfett Disabled"<<std::endl;
#endif

    std::cout << "Moer Engine Editor Starting..." << std::endl;

    Moer::Editor editor;

    editor.Init(argc, argv);

    editor.Run();

    editor.ShutDown();

#ifdef WITH_PROFILE
    shutdown_mimalloc_profiler();
    StopSession(g_session);
#endif

    return 0;
}