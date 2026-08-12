// FBZZ Engine
// PostProcessInspectorWidgets.hpp | fbzz::editor
// Project Settings と PostProcessProfile (.fzdata) で共有するポストプロセス編集 UI
#pragma once

namespace fbzz::renderer { struct PostProcessSettings; }
namespace fbzz::renderer { struct RenderSettings; }

namespace fbzz::editor {

struct PostProcessInspectorResult {
    bool changed = false;
    bool structureChanged = false; // 要素追加/削除など構造変更があった場合 true
};

// PostProcessSettings の既存効果とカスタムパスを一貫した UI で編集する。
// ctx を渡すと TAA/GTAO との排他スロット競合を検出してグレーアウトする。
// PostProcessProfile Inspector など RenderSettings を持たない呼び出し元は nullptr のまま使用可。
PostProcessInspectorResult DrawPostProcessInspector(
    renderer::PostProcessSettings& settings,
    const renderer::RenderSettings* ctx = nullptr);

// 高度グラフィクス設定 (IBL / PCSS / SSR / GTAO / TAA / MotionBlur /
// VolumetricLight / ContactShadows / LensFlare / LUT) を編集する。
// RenderSettings 直下のメンバーを対象とするため引数は RenderSettings 全体を取る。
PostProcessInspectorResult DrawAdvancedGraphicsInspector(renderer::RenderSettings& render);

} // namespace fbzz::editor
