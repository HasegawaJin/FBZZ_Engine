/// @file    VFXLineInspector.hpp
/// @brief   VFX Line (雷・ビーム) の Inspector 部品 — プリセットの列と、形の動くプレビュー
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// プレビューはシーンと同じ GenerateLightning / GenerateBeam で形を作る (VFXLineGeometry.hpp)。
/// 端点だけプレビュー用に横一文字へ固定し、枝の出方・折れ・明滅を数値を触ったその場で見せる。
#pragma once

namespace fbzz::scene { struct VFXLineComponent; }

namespace fbzz::editor {

/// プリセットのボタンを並べる。押されたら値を入れて true (端点・マテリアル・seed は残す)。
bool DrawVFXLinePresetBar(scene::VFXLineComponent& line);

/// 形のプレビュー (時間で動く)。高さは固定。
void DrawVFXLinePreview(const scene::VFXLineComponent& line);

} // namespace fbzz::editor
