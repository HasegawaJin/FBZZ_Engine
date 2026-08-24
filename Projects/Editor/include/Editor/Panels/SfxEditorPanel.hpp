/// @file    SfxEditorPanel.hpp
/// @brief   .synth 手続き効果音をパラメーターと波形プレビューで編集するパネル。
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// WHY 専用パネルにするか:
///   合成パラメーターは Inspector の縦 1 列に並べても「どれを動かすと何が変わるか」が
///   分からない。プリセットから始めて Randomize / Mutate で当たりを引き、波形を見ながら
///   詰める、という探索の手順そのものを UI にする必要がある。
///
/// WHY 状態を自前で持たないか:
///   編集中の値は EditorContext::sfxEditorSpec が唯一の実体で、このパネルはそれを
///   描く面。プリセット読み込み・Randomize・保存などはすべて sfx.* Operator を
///   呼ぶだけにしてあり、人がボタンを押しても AI が editor.op.invoke を呼んでも
///   同じ実装を通る (Docs/design/editor-operator-model.md)。
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

    /// AssetBrowser のダブルクリックから開く。次の描画で読み込む。
    void RequestOpen(const std::string& path) { m_requestedPath = path; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    void LoadRequested(EditorContext& ctx);
    /// spec が変わっていれば、プレビュー表示用の折れ線を作り直す。
    /// 外部 (AI の sfx.set_param 等) からの変更にも追従させるため、
    /// パネル自身の編集フラグではなく spec のハッシュで判定する。
    void EnsureWaveform(const EditorContext& ctx);
    bool DrawParameters(EditorContext& ctx);

    std::string m_requestedPath;

    std::vector<float> m_waveform;
    uint64_t           m_waveformSpecHash = 0;
    bool               m_waveformValid    = false;
    float              m_durationSeconds  = 0.0f;
    size_t             m_sampleCount      = 0;

    bool m_autoPreview = true;
    // 値が変わったが、まだ鳴らしていない。操作を離してから 1 度だけ鳴らす。
    bool m_previewPending = false;
};

} // namespace fbzz::editor
