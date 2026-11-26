/// @file    SfxEditorPanel.hpp
/// @brief   .synth 手続き効果音をパラメーターと波形プレビューで編集するパネル。
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// @note 合成パラメーターは Inspector の縦 1 列では「どれを動かすと何が変わるか」が分からないため、
/// @note プリセットから始めて Randomize / Mutate で当たりを引き波形を見ながら詰める UI にする。
/// @note 編集中の値は EditorContext::sfxEditorSpec が唯一の実体で、このパネルはそれを描く面。
/// @note 読み込み・Randomize・保存は sfx.* Operator を通し、人の操作も AI の editor.op.invoke も
/// @note 同じ実装を通る (Docs/design/editor-operator-model.md)。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::editor {

class SfxEditorPanel final : public IPanel {
public:
    const char* GetWindowName()        const override { return "SFX Editor"; }
    const char* GetMenuCategory()      const override { return "Tools"; }
    bool        GetDefaultVisibility() const override { return false; }
    void OnRegisterOperators(OperatorRegistry& registry) override;
    void OnLoadSettings(const EditorSettings& settings) override;
    void OnSaveSettings(EditorSettings& settings) const override;

    /// @note AssetBrowser のダブルクリックから開く。次の描画で読み込む。
    void RequestOpen(const std::string& path) { m_requestedPath = path; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    void LoadRequested(EditorContext& ctx);
    /// @note spec が変わっていれば、プレビュー表示用の折れ線を作り直す。
    /// @note 外部 (AI の sfx.set_param 等) からの変更にも追従させるため、
    /// @note パネル自身の編集フラグではなく spec のハッシュで判定する。
    void EnsureWaveform(const EditorContext& ctx);
    bool DrawParameters(EditorContext& ctx);

    std::string m_requestedPath; ///< 次に SFX Editor で開く .synth ファイルのパス。

    std::vector<float> m_waveform; ///< 合成した効果音をグラフ表示するための波形サンプル。
    uint64_t           m_waveformSpecHash = 0; ///< 波形を生成したときの合成設定のハッシュ。
    bool               m_waveformValid    = false; ///< 編集中の合成設定に対応する波形表示データを構築済みか。
    float              m_durationSeconds  = 0.0f; ///< 最後に合成した効果音の再生時間 [s]。
    size_t             m_sampleCount      = 0; ///< 最後に合成した効果音のサンプル数。

    bool m_autoPreview = true; ///< 合成設定を変更した後で効果音を自動的に試聴するか。
    bool m_previewPending = false; ///< 値が変わったが、まだ鳴らしていない。操作を離してから 1 度だけ鳴らす。
};

} // namespace fbzz::editor
