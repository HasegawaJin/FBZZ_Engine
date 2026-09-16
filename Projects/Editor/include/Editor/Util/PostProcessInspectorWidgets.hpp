/// @file    PostProcessInspectorWidgets.hpp
/// @brief   PostProcessProfile (.fzdata) のオーバーライド編集 UI。
/// @author  Hasegawa Jin
/// @date    2026-06-22
///
/// WHY 独立したファイルにするか: 「リストの編集」(追加・削除・並べ替え・一時無効化) は
/// Inspector の他のどのパネルとも構造が違い、Inspector 本体へ直接書くと埋もれる。
#pragma once
#include <string>

namespace fbzz::asset { class PostProcessProfile; }
namespace fbzz::renderer { struct RenderSettings; }

namespace fbzz::editor {

struct ImGuiReflector;

// UI の編集結果。呼び出し側は保存タイミングの判断に使う。
struct PostProcessInspectorResult {
    bool changed          = false;  // 何らかの値が変わった
    bool structureChanged = false;  // オーバーライドの追加/削除/並べ替えが起きた
};

// プロファイルのオーバーライドリストを編集する。
// @param reflector 各オーバーライドのパラメーターを描くのに使う。
//                  projectRoot 等が配線済みのものを呼び出し側が用意する。
// WHY パラメーター UI を ImGuiReflector に任せるか:
//     効果ごとに専用の描画関数を書くと、効果を 1 つ足すたびに Engine 側と
//     Editor 側の 2 か所を触ることになる。Reflect() が既にレンジとカラーヒントを
//     持っているので、それを唯一の情報源にすれば追加作業は Engine 側だけで閉じる。
// @param renderSettings 現在のレンダー設定。渡すと「このパイプラインでは効かない」
//                       オーバーライドにその旨を出す。nullptr なら警告を出さない。
// WHY 任意引数か: このウィジェットは .fzdata の編集 UI で、レンダー設定を持たない
//     文脈 (アセット単体のプレビュー等) からも呼べる形を保っておきたい。
PostProcessInspectorResult DrawVolumeOverrideListInspector(
    asset::PostProcessProfile& profile, ImGuiReflector& reflector,
    const renderer::RenderSettings* renderSettings = nullptr);

} // namespace fbzz::editor
