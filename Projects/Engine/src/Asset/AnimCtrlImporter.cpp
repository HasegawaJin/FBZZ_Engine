/// @file    AnimCtrlImporter.cpp
/// @brief   .animctrl TOML → AnimatorControllerAsset ローダー (LoadAnimatorControllerAsset ラッパー)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Engine/Asset/AnimCtrlImporter.hpp>
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Core/Logger.hpp>

namespace fbzz::asset {

std::unique_ptr<AnimatorControllerAsset> AnimCtrlImporter::Import(
    const std::string&         absPath,
    renderer::ResourceManager* /*resources*/)
{
    auto asset = std::make_unique<AnimatorControllerAsset>();
    if (!LoadAnimatorControllerAsset(absPath, *asset)) {
        FBZZ_LOG_WARN("AnimCtrlImporter: failed to load [%s]", absPath.c_str());
        return nullptr;
    }
    return asset;
}

} // namespace fbzz::asset
