#include <filesystem>
namespace Moer {
    class Editor;
    class Launcher {
    public:
        Launcher(const Launcher&)            = delete;
        Launcher(Launcher&&)                 = delete;
        Launcher& operator=(const Launcher&) = delete;
        Launcher& operator=(Launcher&&)      = delete;

    public:
        void Init(const std::filesystem::path&);

        void Run();

        void Quit();

        static Launcher& GetInstance();

    private:
        Launcher();

        Editor* editor;
    };
}// namespace Moer
