#pragma once

#include <memory>
#include <string_view>

namespace Moer {

// A renderer-independent startup window. On Windows the native window and its
// message pump live entirely on a dedicated UI thread. Other platforms expose
// the same API as a safe no-op so startup call sites need no platform guards.
class StartupSplash final {
public:
    StartupSplash();
    ~StartupSplash();

    StartupSplash(const StartupSplash&)            = delete;
    StartupSplash& operator=(const StartupSplash&) = delete;
    StartupSplash(StartupSplash&&)                 = delete;
    StartupSplash& operator=(StartupSplash&&)      = delete;

    // Starts the native UI thread and waits only until window creation has
    // succeeded or failed. Calling Start again while running updates the text.
    [[nodiscard]] bool Start(
        std::string_view title  = "Starting MoerEditor",
        std::string_view detail = "Preparing engine services"
    ) noexcept;

    // Copies the strings before returning; callers may pass temporary data.
    void Update(std::string_view title, std::string_view detail = {}) noexcept;

    // Begins a short, asynchronous fade-out. Destruction always joins the UI
    // thread, even when Finish was not called explicitly.
    void Finish() noexcept;

    // Switches to a persistent error presentation. Call Finish when the error
    // has been reported through the editor's normal error handling path.
    void Fail(std::string_view title, std::string_view detail = {}) noexcept;

    [[nodiscard]] bool IsRunning() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Moer
