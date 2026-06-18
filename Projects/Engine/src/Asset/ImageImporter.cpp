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
#include <filesystem>

namespace fbzz::asset {

std::unique_ptr<TextureAsset> ImageImporter::Import(
    const std::string&         absPath,
    renderer::ResourceManager* resources)
{
    namespace fs = std::filesystem;
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
        // sourcePath を絶対パスに解決 (.tex ファイルからの相対パス)
        if (!asset->sourcePath.empty()) {
            const fs::path dir = util::FileSystem::PathFromUtf8(absPath).parent_path();
            asset->sourcePath = util::FileSystem::PathToUtf8(
                dir / util::FileSystem::PathFromUtf8(asset->sourcePath));
        }
    } else {
        // 生画像: パスからタイプを推定してデフォルト設定を適用
        asset->sourcePath = absPath;
        const TextureType guessed = GuessTextureType(util::FileSystem::GetFilename(absPath));
        asset->settings = DefaultSettingsForType(guessed);
    }

    // GPU テクスチャロード
    if (resources && !asset->sourcePath.empty()) {
        asset->gpuHandle = resources->LoadTexture(asset->sourcePath);
        if (!asset->gpuHandle.IsValid())
            FBZZ_LOG_WARN("ImageImporter: GPU load failed [%s]", asset->sourcePath.c_str());
    }

    return asset;
}

} // namespace fbzz::asset
