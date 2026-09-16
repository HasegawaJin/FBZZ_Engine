/// @file    FlipbookInspector.hpp
/// @brief   .mat の [particle] フリップブック節 — アトラスのプレビュー・コマ範囲の編集・アトラス生成
/// @author  Hasegawa Jin
/// @date    2026-09-11

#pragma once

#include <string>

namespace fbzz::asset { struct MaterialAsset; }
namespace fbzz::renderer { class ResourceManager; class IImGuiRenderer; }

namespace fbzz::editor {

/// albedo のアトラスにグリッドを重ねて見せ、コマ範囲・再生モード・生成ツールを 1 か所で扱う。
///
/// WHY 数値欄だけにしないか:
///   Columns / Rows / Start / End は «絵を見ながら決める値» なのに、以前は数字を打って
///   ゲームを再生するまで結果が分からなかった。1 マスずれた Columns や、行数を超えた
///   End Frame はエラーにならず «なんとなく変な煙» として出るだけなので、目で確かめる
///   手段が無いと気付けない。プレビューは ParticlePass と同じ EvaluateFlipbookFrame() を
///   呼ぶので、ここで見えたコマがそのままゲームで出る。
///
/// @param resources / imguiRenderer アトラスの表示に使う。null なら数値の編集だけ出す
/// @return 値を変更したら true (呼び出し側が dirty 登録と保存を行う)
bool DrawFlipbookInspector(asset::MaterialAsset& material, const std::string& projectRoot,
                           renderer::ResourceManager* resources,
                           renderer::IImGuiRenderer* imguiRenderer);

/// アトラス表示のために覚えているテクスチャ寸法を捨てる。
/// 焼き直しで同じパスのアトラスを上書きした後に呼ぶ (寸法やコマ割りが変わっているため)。
void InvalidateFlipbookAtlasPreview();

} // namespace fbzz::editor
