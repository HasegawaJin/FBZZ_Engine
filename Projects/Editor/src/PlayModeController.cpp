// FBZZ Engine
// PlayModeController.cpp | fbzz::editor
// Play / Pause / Stop の状態管理とシーンスナップショット
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/SceneSerializer.hpp>
#include <Engine/Core/Logger.hpp>

namespace fbzz::editor {

void PlayModeController::Play(scene::Scene& scene)
{
    if (m_state != PlayState::Editor) return;
    if (m_restorePending) return;
    m_snapshot = SceneSerializer::Serialize(scene);
    if (m_snapshot.empty()) {
        FBZZ_LOG_ERROR("PlayMode: snapshot failed");
        return;
    }
    m_state    = PlayState::Playing;
    FBZZ_LOG_INFO("PlayMode: Play");
}

void PlayModeController::Pause()
{
    if (m_state == PlayState::Playing) {
        m_state = PlayState::Paused;
        FBZZ_LOG_INFO("PlayMode: Pause");
    } else if (m_state == PlayState::Paused) {
        m_state = PlayState::Playing;
        FBZZ_LOG_INFO("PlayMode: Resume");
    }
}

void PlayModeController::Stop(scene::Scene& scene)
{
    (void)scene;
    if (m_state == PlayState::Editor) return;
    if (m_snapshot.empty()) {
        FBZZ_LOG_ERROR("PlayMode: snapshot is empty; scene restore skipped");
        m_state = PlayState::Editor;
        return;
    }
    m_restorePending = true;
    FBZZ_LOG_INFO("PlayMode: Stop requested");
}

bool PlayModeController::ApplyPendingRestore(scene::Scene& scene)
{
    if (!m_restorePending) return false;

    if (!SceneSerializer::Deserialize(scene, m_snapshot)) {
        FBZZ_LOG_ERROR("PlayMode: scene restore failed");
        return false;
    }
    m_snapshot.clear();
    m_restorePending = false;
    m_state = PlayState::Editor;
    FBZZ_LOG_INFO("PlayMode: Stop");
    return true;
}

} // namespace fbzz::editor
