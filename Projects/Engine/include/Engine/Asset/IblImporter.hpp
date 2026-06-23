// FBZZ Engine
// IblImporter.hpp | fbzz::asset
// .ibl バイナリ descriptor → IblAsset Runtime ローダー
//
// .ibl ファイルは Editor 側の IblBaker が生成する FzIblHeader を持つバイナリ。
// ヘッダー内の 4 つの DDS パス (相対) を ResourceManager::LoadTexture() で GPU にロードし、
// IblAsset::ResourceHandle として返す。
//
// 既存の DX11Texture::Init() が DDS cubemap を DirectXTex で正しく処理するため、
// このインポーターは DDS のロードを完全に ResourceManager に委譲できる。
#pragma once
#include <Engine/Asset/IAssetImporter.hpp>
#include <Engine/Asset/IblAsset.hpp>

namespace fbzz::asset {

class IblImporter final : public IAssetImporter<IblAsset> {
public:
    [[nodiscard]] std::unique_ptr<IblAsset> Import(
        const std::string&          absPath,
        renderer::ResourceManager*  resources) override;

    std::span<const std::string_view> SupportedExtensions() const override {
        static constexpr std::string_view kExts[] = { ".ibl" };
        return kExts;
    }
};

} // namespace fbzz::asset
