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
    FBZZ_LOG_DEBUG("PlayMode: taking pre-play snapshot");
    m_snapshot = SceneSerializer::Serialize(scene);
    if (m_snapshot.empty()) {
        FBZZ_LOG_ERROR("PlayMode: snapshot failed");
        return;
    }
    m_state    = PlayState::Playing;
    FBZZ_LOG_INFO("PlayMode: → Playing");
}

void PlayModeController::Pause()
{
    if (m_state == PlayState::Playing) {
        m_state = PlayState::Paused;
        FBZZ_LOG_INFO("PlayMode: → Paused");
    } else if (m_state == PlayState::Paused) {
        m_state = PlayState::Playing;
        FBZZ_LOG_INFO("PlayMode: → Playing (resumed)");
    }
}

void PlayModeController::Stop(scene::Scene& scene)
{
    (void)scene;
    if (m_state == PlayState::Editor) return;
    if (m_snapshot.empty()) {
        m_state = PlayState::Editor;
        FBZZ_LOG_WARN("PlayMode: Stop called but snapshot is empty; forced → Editor");
        return;
    }
    m_restorePending = true;
    FBZZ_LOG_DEBUG("PlayMode: Stop requested; restore pending next frame");
}

bool PlayModeController::ApplyPendingRestore(scene::Scene& scene)
{
    if (!m_restorePending) return false;

    FBZZ_LOG_DEBUG("PlayMode: restoring scene from snapshot");
    if (!SceneSerializer::Deserialize(scene, m_snapshot)) {
        FBZZ_LOG_ERROR("PlayMode: scene restore failed; state remains Playing");
        return false;
    }
    m_snapshot.clear();
    m_restorePending = false;
    m_state = PlayState::Editor;
    FBZZ_LOG_INFO("PlayMode: → Editor (scene restored)");
    return true;
}

} // namespace fbzz::editor
