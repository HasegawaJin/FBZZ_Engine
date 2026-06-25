// FBZZ Engine
// ImageImporter.cpp | fbzz::asset
// .tex descriptor / 生画像 → TextureAsset
// .tex: TexDescSerializer でメタを読み、sourcePath の画像を GPU ロード
// 生画像: GuessTextureType で設定を推定して GPU ロード
#include <Engine/Asset/ImageImporter.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <cctype>
#include <utility>

namespace fbzz::asset {

std::unique_ptr<TextureAsset> ImageImporter::Import(
    const std::string&         absPath,
    renderer::ResourceManager* resources)
{
    const std::string ext = [&] {
        std::string e = util::FileSystem::GetExtension(absPath);
        for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e;
    }();

    auto asset = std::make_unique<TextureAsset>();

    if (ext == ".tex") {
        // .tex descriptor をパース
        TexDescSerializer ser;
        if (!ser.Load(absPath, *asset)) {
            FBZZ_LOG_WARN("ImageImporter: .tex parse failed [%s]", absPath.c_str());
            return nullptr;
        }
        // sourcePath を .tex ファイル基準で解決する。
        std::string resolvedSourcePath;
        if (!TexDescSerializer::ResolveSourcePath(absPath, resolvedSourcePath)) return nullptr;
        asset->sourcePath = std::move(resolvedSourcePath);
    } else {
        // 生画像: パスからタイプを推定してデフォルト設定を適用
        asset->sourcePath = absPath;
        const TextureType guessed = GuessTextureType(util::FileSystem::GetFilename(absPath));
        asset->settings = DefaultSettingsForType(guessed);
    }

    // GPU テクスチャロード
    if (resources && !asset->sourcePath.empty()) {
        // .tex 自身をキャッシュキーにすることで descriptor の明示リロードを可能にする。
        const std::string& gpuLoadPath = ext == ".tex" ? absPath : asset->sourcePath;
        asset->gpuHandle = resources->LoadTexture(gpuLoadPath);
        if (!asset->gpuHandle.IsValid())
            FBZZ_LOG_WARN("ImageImporter: GPU load failed [%s]", asset->sourcePath.c_str());
    }

    return asset;
}

} // namespace fbzz::asset
