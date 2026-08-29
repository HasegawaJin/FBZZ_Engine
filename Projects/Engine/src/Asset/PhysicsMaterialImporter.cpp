/// @file    PhysicsMaterialImporter.cpp
/// @brief   .physmat TOML → PhysicsMaterialAsset。
/// @author  Hasegawa Jin
/// @date    2026-08-16
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
