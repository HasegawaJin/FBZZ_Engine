/// @file    ImageImporter.cpp
/// @brief   生画像 → TextureAsset。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// 元画像の隣の "<画像>.meta" があれば TexDescSerializer で設定を読み、GPU ロード
/// サイドカーが無ければ GuessTextureType で設定を推定して GPU ロード
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
    auto asset = std::make_unique<TextureAsset>();

    /// @note 元画像パスを確定する。".meta" を直接渡された場合も元画像へ解決する。
    std::string sourcePath;
    if (!TexDescSerializer::ResolveSourcePath(absPath, sourcePath) || sourcePath.empty()) {
        FBZZ_LOG_WARN("ImageImporter: source resolution failed [%s]", absPath.c_str());
        return nullptr;
    }
    asset->sourcePath = sourcePath;

    /// @note 元画像の隣にある "<画像>.meta" サイドカーがあればインポート設定を読む。
    ///       無ければファイル名からタイプを推定してデフォルト設定を適用する。
    const std::string metaPath = sourcePath + ".meta";
    bool settingsLoaded = false;
    if (util::FileSystem::Exists(metaPath)) {
        /// @note guid のみの .meta ([texture] セクションなし) は false が返り、デフォルト設定に落ちる。
        ///       破損 TOML の警告は TexDescSerializer 側が出すためここでは扱わない。
        TexDescSerializer ser;
        settingsLoaded = ser.Load(metaPath, *asset);
    }
    if (!settingsLoaded) {
        const TextureType guessed = GuessTextureType(util::FileSystem::GetFilename(sourcePath));
        asset->settings = DefaultSettingsForType(guessed);
    }

    /// @note GPU テクスチャロード (キャッシュキーは常に元画像パスで安定させる)
    if (resources) {
        asset->gpuHandle = resources->LoadTexture(asset->sourcePath);
        if (!asset->gpuHandle.IsValid())
            FBZZ_LOG_WARN("ImageImporter: GPU load failed [%s]", asset->sourcePath.c_str());
    }

    return asset;
}

} // namespace fbzz::asset
