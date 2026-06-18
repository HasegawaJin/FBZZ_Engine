// FBZZ Engine
// MatAssetImporter.cpp | fbzz::asset
// .mat TOML → MaterialAsset ローダー (LoadMaterialAssetFromFile ラッパー)
#include <Engine/Asset/MatAssetImporter.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Core/Logger.hpp>

namespace fbzz::asset {

std::unique_ptr<MaterialAsset> MatAssetImporter::Import(
    const std::string&         absPath,
    renderer::ResourceManager* /*resources*/)
{
    auto mat = std::make_unique<MaterialAsset>();
    if (!LoadMaterialAssetFromFile(absPath, *mat)) {
        FBZZ_LOG_WARN("MatAssetImporter: failed to load [%s]", absPath.c_str());
        return nullptr;
    }
    return mat;
}

} // namespace fbzz::asset
