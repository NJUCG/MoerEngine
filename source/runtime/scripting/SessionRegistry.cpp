#include "scripting/SessionRegistry.h"

#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace Moer::scripting {

py::dict& ScriptSession::RequireGlobals() {
    if (!globals.has_value()) {
        throw std::runtime_error("ScriptSession globals is not initialized.");
    }

    return *globals;
}

const py::dict& ScriptSession::RequireGlobals() const {
    if (!globals.has_value()) {
        throw std::runtime_error("ScriptSession globals is not initialized.");
    }

    return *globals;
}

namespace {

py::dict CloneDict(const std::optional<py::dict>& source) {
    if (!source.has_value() || !source->ptr()) {
        return py::dict();
    }

    return source->attr("copy")().cast<py::dict>();
}

} // namespace

void SessionRegistry::Reset(const py::dict& shared_globals, const py::dict& session_seed_globals) {
    py::gil_scoped_acquire guard;

    Clear();

    m_shared_global_session.session_id = "<shared-global>";
    m_shared_global_session.globals.emplace(shared_globals);
    m_session_seed_globals = session_seed_globals;
}

void SessionRegistry::Clear() {
    py::gil_scoped_acquire guard;

    m_named_sessions.clear();
    m_shared_global_session = {};
    m_session_seed_globals.reset();
}

ScriptSession& SessionRegistry::GetSharedGlobalSession() {
    return m_shared_global_session;
}

ScriptSession& SessionRegistry::GetOrCreateNamedSession(std::string_view session_id) {
    py::gil_scoped_acquire guard;

    auto [it, inserted] = m_named_sessions.try_emplace(std::string(session_id));
    if (inserted) {
        it->second.session_id = it->first;
        it->second.globals.emplace(CloneSessionSeedGlobals());
    }

    return it->second;
}

ScriptSession SessionRegistry::CreateStatelessSession() const {
    py::gil_scoped_acquire guard;

    ScriptSession session;
    session.session_id = "<stateless>";
    session.globals.emplace(CloneSessionSeedGlobals());
    return session;
}

py::dict SessionRegistry::CloneSessionSeedGlobals() const {
    return CloneDict(m_session_seed_globals);
}

} // namespace Moer::scripting
