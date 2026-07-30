#pragma once

#include <memory>

namespace Moer::ProfileDump {
class ProfileDocumentLoader;
}

namespace Moer {

// Editor-embedded, offline viewer for immutable ProfileDump documents. The
// implementation is hidden so ProfileConsumer does not leak through public
// Editor headers.
class ProfileViewerUI final {
public:
    explicit ProfileViewerUI(ProfileDump::ProfileDocumentLoader& _loader);
    ~ProfileViewerUI();

    ProfileViewerUI(const ProfileViewerUI&)            = delete;
    ProfileViewerUI& operator=(const ProfileViewerUI&) = delete;
    ProfileViewerUI(ProfileViewerUI&&)                 = delete;
    ProfileViewerUI& operator=(ProfileViewerUI&&)      = delete;

    void ShowWindow(bool* _open);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

} // namespace Moer
