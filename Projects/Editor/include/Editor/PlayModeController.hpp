// FBZZ Engine
// PlayModeController.hpp | fbzz::editor
// エディター内のゲームループ実行状態を管理する
#pragma once
#include <string>

namespace fbzz::scene { class Scene; }

namespace fbzz::editor {

enum class PlayState { Editor, Playing, Paused };

class PlayModeController {
public:
    // シーンを TOML スナップショットに保存してからゲームループを開始する
    void Play(scene::Scene& scene);
    void Pause();
    // スナップショットからシーンを復元してエディターモードに戻る
    void Stop(scene::Scene& scene);
    // Stop() はスナップショット復元を即座に行わず、このメソッドを次フレームに呼ばせる。
    // Stop() 呼び出し時は Update ループが実行中の可能性があり、その最中にシーンを書き換えると
    // イテレーション中のポインタが無効になる恐れがあるため。
    bool ApplyPendingRestore(scene::Scene& scene);

    PlayState GetState() const  { return m_state; }
    bool IsPlaying()    const   { return m_state == PlayState::Playing; }
    bool IsPaused()     const   { return m_state == PlayState::Paused; }
    bool IsInEditor()   const   { return m_state == PlayState::Editor; }
    bool HasPendingRestore() const { return m_restorePending; }

private:
    PlayState   m_state    = PlayState::Editor;
    std::string m_snapshot; // TOML 文字列でシーン状態を保存
    bool        m_restorePending = false;
};

} // namespace fbzz::editor
