/// @file    VectorFieldImporter.cpp
/// @brief   速度場 PNG / .fga を CPU の格子へ読み込む。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/VectorFieldImporter.hpp>

#include <Engine/Asset/VectorFieldFile.hpp>
#include <Engine/Core/Logger.hpp>
#include <algorithm>
#include <cctype>

namespace fbzz::asset {

std::unique_ptr<fluid::VectorFieldAsset> VectorFieldImporter::Import(
    const std::string& absPath, renderer::ResourceManager* resources)
{
    std::string extension;
    if (const size_t dot = absPath.find_last_of('.'); dot != std::string::npos)
        extension = absPath.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (extension != ".png" && extension != ".fga") return nullptr;

    auto field = std::make_unique<fluid::VectorFieldAsset>();
    const bool loaded = extension == ".fga" ? ImportFgaFile(absPath, *field)
                                            : LoadVectorFieldFile(absPath, *field);
    if (!loaded) return nullptr;

    /// @note 解像度と量子化はここで必ず揃える。resources の有無に依らないので、静的検査も
    ///       テストも実行時と同じ場を見る。GPU への常駐は VelocityFieldAtlas が
    ///       «実際に使われたとき» に行う。
    (void)resources;
    /// @note PNG の32³は保存時に量子化済み。特にゼロ場の量子化残差を再正規化して増減させない。
    if (extension != ".png" || field->sizeX != fluid::kVelocityFieldTileResolution
        || field->sizeY != fluid::kVelocityFieldTileResolution || field->sizeZ != fluid::kVelocityFieldTileResolution)
        fluid::NormalizeVectorField(*field);
    return field;
}

} // namespace fbzz::asset
