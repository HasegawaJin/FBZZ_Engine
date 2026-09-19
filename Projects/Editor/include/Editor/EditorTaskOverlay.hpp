/// @file    EditorTaskOverlay.hpp
/// @brief   重い処理の実行中に全 ImGui 入力をブロックするモーダルオーバーレイ。
/// @author  Hasegawa Jin
/// @date    2026-06-06
///
/// @note FBX インポート等の秒単位処理中にシーン操作が入ると中間状態を壊すため、完了まで全操作を遮断する。分単位の BuildPipeline は別のノンブロッキング進捗パネルで扱う。
#pragma once

namespace fbzz::editor {

class EditorTaskOverlay {
public:
    /// 処理開始。オーバーレイを表示してすべての ImGui 入力をブロックする。
    static void Begin(const char* taskName);

    /// 処理中のステップ説明を更新する。Begin〜End の間に何度でも呼べる。
    static void SetStatus(const char* status);

    /// 進捗を 0.0〜1.0 で設定する。-1.0f を渡すとループアニメーション表示になる。
    static void SetProgress(float progress);

    /// 処理完了。次の Render() 呼び出しでオーバーレイを閉じる。
    static void End();

    /// EditorApp::RenderPanels() 末尾から毎フレーム呼ぶ。
    /// ImGui::BeginPopupModal を内部で管理するため、すべての通常ウィンドウの後に呼ぶこと。
    static void Render();

    [[nodiscard]] static bool IsActive() { return s_active; }

private:
    static bool  s_active;
    static bool  s_needOpen;
    static char  s_taskName[128];
    static char  s_status[256];
    static float s_progress;
};

} // namespace fbzz::editor
