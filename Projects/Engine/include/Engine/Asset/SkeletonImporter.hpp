// FBZZ Engine
// SkeletonImporter.hpp | fbzz::asset
// .skel バイナリ (FzSkelHeader フォーマット) → Skeleton デシリアライザ。
// WHY: SkelSubExporter が FBX インポート時に書き出した .skel ファイルを
//      ランタイムで読み込み、メッシュモデルとは独立してスケルトン構造を参照できるようにする。
//      スキンなし FBX (アニメーション専用) からのインポートでも骨ノード階層を提供できる。
#pragma once
#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/Skeleton.hpp>

namespace fbzz::asset {

class SkeletonImporter : public IAssetImporter<Skeleton> {
public:
    // .skel ファイルを読み込んで Skeleton を返す。失敗時は nullptr。
    [[nodiscard]] std::unique_ptr<Skeleton> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    std::span<const std::string_view> SupportedExtensions() const override;
};

} // namespace fbzz::asset
