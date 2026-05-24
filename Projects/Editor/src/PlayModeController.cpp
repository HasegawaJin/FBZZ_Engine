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
}

void PlayModeController::Pause()
{
    if (m_state == PlayState::Playing) {
        m_state = PlayState::Paused;
    } else if (m_state == PlayState::Paused) {
        m_state = PlayState::Playing;
    }
}

void PlayModeController::Stop(scene::Scene& scene)
{
    (void)scene;
    if (m_state == PlayState::Editor) return;
    if (m_snapshot.empty()) {
        m_state = PlayState::Editor;
        return;
    }
    // ここで直接 Deserialize しない。Stop() は Update/Render ループの途中から呼ばれる可能性があり、
    // 即座にシーンを書き換えるとその後のフレーム処理中のポインタが無効になる。
    // main ループが ApplyPendingRestore() を次フレーム開始前に呼ぶことで安全に復元する。
    m_restorePending = true;
}

bool PlayModeController::ApplyPendingRestore(scene::Scene& scene)
{
    if (!m_restorePending) return false;

    if (!SceneSerializer::Deserialize(scene, m_snapshot)) {
        return false;
    }
    m_snapshot.clear();
    m_restorePending = false;
    m_state = PlayState::Editor;
    return true;
}

} // namespace fbzz::editor
