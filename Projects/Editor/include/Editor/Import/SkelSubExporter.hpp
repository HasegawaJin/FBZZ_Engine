/// @file    SkelSubExporter.hpp
/// @brief   FBX インポートパイプラインのスケルトン書き出しサブエクスポーター。
/// @author  Hasegawa Jin
/// @date    2026-06-19
///
/// skinned / animation 専用 FBX の両方からシーンノード階層を .skel ファイルに書き出す。
/// WHY: メッシュなしの Mixamo アニメーション FBX からもスケルトンを独立ファイルとして
/// 取り出し、AnimatorSystem がスキンモデルなしでもボーン階層を参照できるようにする。
/// Player.fzasset はスキン付きで骨構造を保持するが、他のシーンや将来の構成では
/// メッシュを持たない .skel だけから骨階層を参照できると設計が柔軟になる。
#pragma once
#include <Editor/Import/IFbxSubExporter.hpp>

namespace fbzz::editor {

class SkelSubExporter : public IFbxSubExporter {
public:
    [[nodiscard]] bool Export(FbxImportContext& ctx) override;
    const char* Name() const override { return "SkelSubExporter"; }
};

} // namespace fbzz::editor
