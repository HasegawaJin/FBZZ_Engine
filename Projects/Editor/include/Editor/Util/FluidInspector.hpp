/// @file    FluidInspector.hpp
/// @brief   .fluid (流体エフェクトのレシピ) の Inspector — 要約・焼き・焼いた出力への導線 (編集は Fluid Editor)
/// @author  Hasegawa Jin
/// @date    2026-09-11

#pragma once

#include <string>

namespace fbzz::renderer { class ResourceManager; class IImGuiRenderer; }

namespace fbzz::editor {

struct EditorContext;

/// Inspector の Bake と «Open in Fluid Editor» が使う文脈 (FluidBakeService・要求欄) を繋ぐ。
/// EditorApp が起動時に繋ぎ、終了時に外す。null の間はどちらも押せない (要約は出る)。
void BindFluidInspectorContext(EditorContext* ctx);

/// .fluid を開いたときの Inspector 本体 (要約カード)。レシピの編集は Fluid Editor が持ち、ここは書き換えない。
///
/// 焼いた結果 (フリップブック / Motion Vector / .vfield) は .fluid の隣に置き、
/// «Create / Update Particle Material» で同名の .mat を作る (既にあれば焼いた設定へ追従させる)。
/// WHY .mat まで作るか: フリップブックのコマ割り・再生モード・Motion Vector の強さは焼いた側しか
///     正しい値を知らない。人に写させると «8x8 で焼いたのに 4x4 のまま» が起き、エラーにもならない。
/// @param absPath   開いている .fluid の実パス
/// @param resources / imguiRenderer 今は使わない (プレビューは Fluid Editor へ移った。呼び手の互換のため残す)
/// @param volumeBakeRequest 今は使わない (.fluid の 3D は Fluid Editor が持つ。呼び手の互換のため残す)
void DrawFluidAssetInspector(const std::string& absPath,
                             renderer::ResourceManager* resources = nullptr,
                             renderer::IImGuiRenderer* imguiRenderer = nullptr,
                             std::string* volumeBakeRequest = nullptr);

} // namespace fbzz::editor
