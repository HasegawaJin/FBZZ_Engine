/// @file    FluidInspector.hpp
/// @brief   .fluid (流体エフェクトのレシピ) の Inspector — 要約・焼き・焼いた出力への導線 (編集は Fluid Editor)
/// @author  Hasegawa Jin
/// @date    2026-09-11

#pragma once

#include <string>

namespace fbzz::renderer { class ResourceManager; class IImGuiRenderer; }

namespace fbzz::editor {

struct EditorContext;

/// @brief Inspector の Bake と «Open in Fluid Editor» が使う文脈 (FluidBakeService・要求欄) を繋ぐ。
/// @brief EditorApp が起動時に繋ぎ、終了時に外す。null の間はどちらも押せない (要約は出る)。
void BindFluidInspectorContext(EditorContext* ctx);

/// @brief .fluid を開いたときの Inspector 本体 (要約カード)。レシピの編集は Fluid Editor が持ち、ここは書き換えない。
/// @note 焼いた結果 (フリップブック/Motion Vector/速度場 PNG) は .fluid の隣に置き、Create/Update Particle
///       Material で同名の .mat へ反映する。コマ割り・再生モード・強さは焼いた側にしか正しい値が無く、
///       人が写すと食い違ってもエラーにならない。
/// @param[in] absPath 開いている .fluid の実パス
/// @param[in] resources 互換性のため受け取る描画リソース。関数内では使用しない。
/// @param[in] imguiRenderer 互換性のため受け取る描画インターフェース。関数内では使用しない。
/// @param[in] volumeBakeRequest 互換性のため受け取るベイク要求先。関数内では読み書きしない。
void DrawFluidAssetInspector(const std::string& absPath,
                             renderer::ResourceManager* resources = nullptr,
                             renderer::IImGuiRenderer* imguiRenderer = nullptr,
                             std::string* volumeBakeRequest = nullptr);

} // namespace fbzz::editor
