/// @file    SequenceImporter.cpp
/// @brief   .sequence を AssetManager のストアへ載せる
/// @author  Hasegawa Jin
/// @date    2026-08-26
#include <Engine/Asset/SequenceImporter.hpp>

#include <Engine/Core/Logger.hpp>

namespace fbzz::asset {

std::unique_ptr<SequenceAsset> SequenceImporter::Import(
    const std::string&         absPath,
    renderer::ResourceManager* /*resources*/)
{
    auto asset = std::make_unique<SequenceAsset>();
    if (!LoadSequenceAsset(absPath, *asset)) {
        FBZZ_LOG_WARN("SequenceImporter: failed to load [%s]", absPath.c_str());
        return nullptr;
    }
    return asset;
}

} // namespace fbzz::asset
