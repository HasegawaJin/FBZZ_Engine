// FBZZ Engine
// PhysicsMaterialImporter.cpp | fbzz::asset
// .physmat TOML → PhysicsMaterialAsset
#include <Engine/Asset/PhysicsMaterialImporter.hpp>
#include <Engine/Core/Logger.hpp>

namespace fbzz::asset {

std::unique_ptr<PhysicsMaterialAsset> PhysicsMaterialImporter::Import(
    const std::string&         absPath,
    renderer::ResourceManager* /*resources*/)
{
    auto asset = std::make_unique<PhysicsMaterialAsset>();
    if (!LoadPhysicsMaterialAssetFromFile(absPath, *asset)) {
        FBZZ_LOG_ERROR("PhysicsMaterialImporter: load failed [%s]", absPath.c_str());
        return nullptr;
    }
    return asset;
}

} // namespace fbzz::asset
