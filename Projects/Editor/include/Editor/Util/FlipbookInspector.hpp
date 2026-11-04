/// @file    FlipbookInspector.hpp
/// @brief   .mat の [particle] フリップブック節 — アトラスのプレビュー・コマ範囲の編集・アトラス生成
/// @author  Hasegawa Jin
/// @date    2026-09-11

#pragma once

#include <string>
#include <vector>

namespace fbzz::asset { struct MaterialAssetAuthoring; }
namespace fbzz::renderer { class ResourceManager; class IImGuiRenderer; }

namespace fbzz::editor {

/// @note albedo のアトラスにグリッドを重ねて見せ、コマ範囲・再生モード・生成ツールを 1 か所で扱う。
/// @note Columns/Rows/Start/End はずれてもエラーにならず見た目だけがおかしくなる値。プレビューは
/// @note       ParticlePass と同じ EvaluateFlipbookFrame() を使うため、ここで見えたコマがそのままゲームに出る。
/// @param resources / imguiRenderer アトラスの表示に使う。null なら数値の編集だけ出す
/// @return 値を変更したら true (呼び出し側が dirty 登録と保存を行う)
bool DrawFlipbookInspector(asset::MaterialAssetAuthoring& material, const std::string& projectRoot,
                           renderer::ResourceManager* resources,
                           renderer::IImGuiRenderer* imguiRenderer);

/// @note アトラス表示のために覚えているテクスチャ寸法を捨てる。
/// @note 焼き直しで同じパスのアトラスを上書きした後に呼ぶ (寸法やコマ割りが変わっているため)。
void InvalidateFlipbookAtlasPreview();
/// @note 同じ接頭辞・拡張子の連番を数値順に並べる。失敗理由はerrorへ返す。
std::vector<std::string> CollectFlipbookSequenceFrames(const std::string& firstFramePath, std::string& error);

} /// @note namespace fbzz::editor
