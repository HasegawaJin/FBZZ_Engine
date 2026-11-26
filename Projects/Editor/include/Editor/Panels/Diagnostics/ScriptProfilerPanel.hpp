/// @file    ScriptProfilerPanel.hpp
/// @brief   スクリプトの型、callback、実体と呼び出しツリーの計測表示。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#pragma once
#include <Editor/Panels/IPanel.hpp>
namespace fbzz::editor {
class ScriptProfilerPanel final : public IPanel {
public:
    const char* GetWindowName() const override { return "Script Profiler"; }
    bool GetDefaultVisibility() const override { return false; }
    void OnLoadSettings(const EditorSettings& settings) override;
    void OnSaveSettings(EditorSettings& settings) const override;
protected:
    void OnRenderContent(EditorContext& ctx) override;
private:
    int m_group = 0; ///< C# の計測結果の行をまとめる分類。
    int m_mode = 0; ///< Script Profiler の現在の表示モード。
    int m_range = 1; ///< Script Profiler が集計する範囲の選択値。
    bool m_callTree = false; ///< Script Profiler の呼び出しツリー表示を使うか。
    char m_name[128] = {}; ///< 計測結果を名前で絞る検索入力バッファ。
    char m_type[128] = {}; ///< 計測結果をスクリプト型名で絞る検索入力バッファ。
    char m_instance[128] = {}; ///< 計測結果をスクリプト個体で絞る検索入力バッファ。
    char m_callback[128] = {}; ///< 計測結果をコールバックで絞る検索入力バッファ。
    char m_scene[32] = {}; ///< 計測結果を対象シーンで絞る検索入力バッファ。
};
}
