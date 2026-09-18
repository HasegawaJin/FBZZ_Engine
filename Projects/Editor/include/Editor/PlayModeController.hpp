/// @file    PlayModeController.hpp
/// @brief   エディター内のゲームループ実行状態を管理する。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <string>

namespace fbzz::scene { class Scene; }

namespace fbzz::editor {

enum class PlayState { Editor, Playing, Paused };

class PlayModeController {
public:
    /// シーンを TOML スナップショットに保存してからゲームループを開始する
    void Play(scene::Scene& scene);
    void Pause();
    /// スナップショットからシーンを復元してエディターモードに戻る
    void Stop(scene::Scene& scene);
    /// Stop() はスナップショット復元を即座に行わず、このメソッドを次フレームに呼ばせる。
    /// Stop() 呼び出し時は Update ループが実行中の可能性があり、その最中にシーンを書き換えると
    /// イテレーション中のポインタが無効になる恐れがあるため。
    bool ApplyPendingRestore(scene::Scene& scene);

    /// Paused 中に 1 フレームだけゲームシステムを走らせる
    void RequestStep() { if (m_state == PlayState::Paused) m_stepRequested = true; }
    /// main ループが毎フレーム呼び、消費したら true を返す (一度だけ true)
    bool ConsumeStep() { if (!m_stepRequested) return false; m_stepRequested = false; return true; }

    PlayState GetState() const  { return m_state; }
    bool IsPlaying()    const   { return m_state == PlayState::Playing; }
    bool IsPaused()     const   { return m_state == PlayState::Paused; }
    bool IsInEditor()   const   { return m_state == PlayState::Editor; }
    bool HasPendingRestore() const { return m_restorePending; }

private:
    PlayState   m_state    = PlayState::Editor;
    std::string m_snapshot; ///< TOML 文字列でシーン状態を保存
    bool        m_restorePending = false;
    bool        m_stepRequested  = false;
};

} // namespace fbzz::editor
