/// @file    IblImporter.cpp
/// @brief   .ibl バイナリ → IblAsset (4 つの DDS を GPU にロード)。
/// @author  Hasegawa Jin
/// @date    2026-06-23
#include <Engine/Asset/IblImporter.hpp>
#include <Engine/Asset/BinaryReader.hpp>
#include <Engine/Format/FzAssetFormat.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <filesystem>
#include <cstring>

namespace fbzz::asset {

std::unique_ptr<IblAsset> IblImporter::Import(
    const std::string&         absPath,
    renderer::ResourceManager* resources)
{
    if (!resources) {
        FBZZ_LOG_WARN("IblImporter: ResourceManager が null です [%s]", absPath.c_str());
        return nullptr;
    }

    BinaryReader reader;
    if (!reader.Open(absPath)) {
        FBZZ_LOG_ERROR("IblImporter: ファイルを開けません [%s]", absPath.c_str());
        return nullptr;
    }

    FzIblHeader header{};
    if (!reader.ReadBytes(&header, sizeof(header))) {
        FBZZ_LOG_ERROR("IblImporter: ヘッダー読み込み失敗 [%s]", absPath.c_str());
        return nullptr;
    }

    // マジック検証
    if (std::strncmp(header.magic, "FZIBL", 5) != 0) {
        FBZZ_LOG_ERROR("IblImporter: 不正なマジック番号 [%s]", absPath.c_str());
        return nullptr;
    }

    // DDS パスは .ibl と同一ディレクトリからの相対パスで格納されている
    namespace fs = std::filesystem;
    const fs::path dir = util::FileSystem::PathFromUtf8(absPath).parent_path();

    auto resolve = [&](const char* rel) -> std::string {
        return util::FileSystem::PathToUtf8(dir / util::FileSystem::PathFromUtf8(rel));
    };

    const std::string envPath      = resolve(header.envCubemapPath);
    const std::string irrPath      = resolve(header.irradiancePath);
    const std::string prefilterPath = resolve(header.prefilteredPath);
    const std::string brdfPath     = resolve(header.brdfLutPath);

    auto asset = std::make_unique<IblAsset>();
    asset->prefilteredMipCount = header.prefilteredMipCount;

    // 既存の LoadTexture が DDS キューブマップを DirectXTex で正しく処理する
    asset->environmentCubemap = resources->LoadTexture(envPath);
    asset->irradianceCubemap  = resources->LoadTexture(irrPath);
    asset->prefilteredCubemap = resources->LoadTexture(prefilterPath);
    asset->brdfLut            = resources->LoadTexture(brdfPath);

    if (!asset->IsValid()) {
        FBZZ_LOG_WARN("IblImporter: 一部の DDS ロードに失敗しました [%s]", absPath.c_str());
    }

    return asset;
}

} // namespace fbzz::asset
