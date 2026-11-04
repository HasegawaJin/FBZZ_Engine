/// @file    SkeletonImporter.hpp
/// @brief   .skel バイナリ (FzSkelHeader フォーマット) → Skeleton デシリアライザ。
/// @author  Hasegawa Jin
/// @date    2026-06-19

/// @note SkelSubExporter が書き出した .skel を独立して読み込み、メッシュモデルと切り離してスケルトン
/// @note 構造を参照できるようにする。スキンなし FBX (アニメーション専用) でも骨ノード階層を提供できる。
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/Skeleton.hpp>

namespace fbzz::asset {

class SkeletonImporter : public IAssetImporter<Skeleton> {
public:
    /// @note .skel ファイルを読み込んで Skeleton を返す。失敗時は nullptr。
    [[nodiscard]] FBZZ_ENGINE_API std::unique_ptr<Skeleton> Import(
        const std::string&         absPath,
        renderer::ResourceManager* resources) override;

    FBZZ_ENGINE_API std::span<const std::string_view> SupportedExtensions() const override;
};

} /// @note namespace fbzz::asset
