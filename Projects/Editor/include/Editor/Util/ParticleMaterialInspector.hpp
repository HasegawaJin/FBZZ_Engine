/// @file    ParticleMaterialInspector.hpp
/// @brief   .mat の [particle] テーブル (パーティクルの «見た目») 用 Inspector セクション
/// @author  Hasegawa Jin
/// @date    2026-08-24

#pragma once

#include <string>

namespace fbzz::asset { struct MaterialAsset; }
namespace fbzz::renderer { class ResourceManager; class IImGuiRenderer; }

namespace fbzz::editor {

/// render_path = "particle" の .mat に対して、見た目一式を編集するセクションを描く。
/// @param projectRoot テクスチャ生成の出力先とパス解決に使う (EditorContext::projectRoot)
/// @param resources / imguiRenderer フリップブックのアトラス表示に使う。null なら数値の編集だけ出す
/// @return 値を変更したら true (呼び出し側が dirty 登録と保存を行う)
///
/// WHY ParticleEmitter 側に置かないか:
///   ブレンド・フリップブック・歪み・煙・自己影は «その素材がどう見えるか» で、
///   同じ .mat を使う全エミッターで共有される。コンポーネントが持っていた頃は
///   素材を差し替えるたびに 34 項目を貼り直す必要があり、しかも .mat 側の値が
///   毎フレーム上書きしてくるため «Inspector で変えても戻る» が起きていた。
bool DrawParticleMaterialInspector(asset::MaterialAsset& material, const std::string& projectRoot,
                                   renderer::ResourceManager* resources = nullptr,
                                   renderer::IImGuiRenderer* imguiRenderer = nullptr);

} // namespace fbzz::editor
