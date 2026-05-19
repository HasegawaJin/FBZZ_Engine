// FBZZ Engine
// PlayModeController.cpp | fbzz::editor
// Play / Pause / Stop の状態管理とシーンスナップショット
#include <editor/PlayModeController.hpp>
#include <editor/Util/SceneSerializer.hpp>
#include <engine/Core/Logger.hpp>

namespace fbzz::editor {

void PlayModeController::Play(scene::Scene& scene)
{
    if (m_state != PlayState::Editor) return;
    m_snapshot = SceneSerializer::Serialize(scene);
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
    if (m_state == PlayState::Editor) return;
    SceneSerializer::Deserialize(scene, m_snapshot);
    m_snapshot.clear();
    m_state = PlayState::Editor;
    FBZZ_LOG_INFO("PlayMode: Stop");
}

} // namespace fbzz::editor
