// FBZZ Engine
// PlayModeController.cpp | fbzz::editor
// Play / Pause / Stop の状態管理とシーンスナップショット
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Engine/Core/Cursor.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Input/InputActionMap.hpp>

namespace fbzz::editor {

void PlayModeController::Play(scene::Scene& scene)
{
    if (m_state != PlayState::Editor) return;
    if (m_restorePending) return;
    FBZZ_LOG_DEBUG("PlayMode: taking pre-play snapshot");
    m_snapshot = SceneIO::Serialize(scene);
    if (m_snapshot.empty()) {
        FBZZ_LOG_ERROR("PlayMode: snapshot failed");
        return;
    }
    m_state    = PlayState::Playing;
    // WHY: Editor は同一プロセス内で Play を繰り返すため、前セッションの押下状態を新しい実行へ持ち越さない。
    input::Input::Reset();
    // ゲーム入力のアクション層は Play 中のみ有効にする。
    // WHY: 編集中も評価していると、シーンビューで W を押しただけで
    //      "MoveY" が立ち、ビューポート操作とゲーム操作が二重発火する。
    input::InputActionMap::SetEnabled(true);
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
    // WHY: Script が PlayMode 中にカーソルを非表示・拘束したまま Stop されても、
    //      Editor 操作へ戻れるように PlayMode 終了要求時点で必ず復元する。
    core::Cursor::ResetForEditor();
    // Stop 要求時点でゲーム入力を止める。
    // WHY 復元完了 (ApplyPendingRestore) を待たないか: 復元は次フレームに走るため、
    //     その 1 フレームぶんゲーム入力が生き残り、Stop クリック直後の操作が
    //     破棄されるはずのシーンへ届いてしまう。
    input::InputActionMap::SetEnabled(false);
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
    if (!SceneIO::Deserialize(scene, m_snapshot)) {
        FBZZ_LOG_ERROR("PlayMode: scene restore failed; state remains Playing");
        return false;
    }
    m_snapshot.clear();
    m_restorePending = false;
    m_state = PlayState::Editor;
    // WHAT: Stop クリックや GameView 操作中のマウス・キー状態を Editor モードへ残さない。
    input::Input::Reset();
    core::Cursor::ResetForEditor();
    FBZZ_LOG_INFO("PlayMode: → Editor (scene restored)");
    return true;
}

} // namespace fbzz::editor
