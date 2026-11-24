/// @file    PostProcessInspectorWidgets.hpp
/// @brief   PostProcessProfile (.fzdata) のオーバーライド編集 UI。
/// @author  Hasegawa Jin
/// @date    2026-06-22
///
/// リストの編集 (追加・削除・並べ替え・一時無効化) は Inspector の他パネルと構造が違うため
/// 独立ファイルにする。
#pragma once
#include <string>

namespace fbzz::asset { class PostProcessProfile; }
namespace fbzz::renderer { struct RenderSettings; }

namespace fbzz::editor {

struct ImGuiReflector;

/// UI の編集結果。呼び出し側は保存タイミングの判断に使う。
struct PostProcessInspectorResult {
    bool changed          = false;  ///< 何らかの値が変わった
    bool structureChanged = false;  ///< オーバーライドの追加/削除/並べ替えが起きた
};

/// プロファイルのオーバーライドリストを編集する。
/// @param reflector 各オーバーライドの描画に使う。projectRoot 等は呼び出し側で配線済み。
/// @note パラメーター UI は ImGuiReflector に任せる。効果ごとに専用関数を書くと Engine/Editor
///       の 2 か所を触ることになるため、Reflect() のレンジ・カラーヒントを唯一の情報源にする。
/// @param renderSettings 現在のレンダー設定。渡すと非対応オーバーライドにその旨を出す。null なら警告なし。
/// @note null を許すのは、.fzdata 編集 UI としてレンダー設定を持たない文脈 (単体プレビュー等) からも呼べるため。
PostProcessInspectorResult DrawVolumeOverrideListInspector(
    asset::PostProcessProfile& profile, ImGuiReflector& reflector,
    const renderer::RenderSettings* renderSettings = nullptr);

} // namespace fbzz::editor
