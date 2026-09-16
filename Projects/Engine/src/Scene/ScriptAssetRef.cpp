/// @file    ScriptAssetRef.cpp
/// @brief   Script用Asset参照のGUID・path相互解決。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Engine/Scene/ScriptAssetRef.hpp>

#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Util/FileSystem.hpp>

namespace fbzz::scene {

void ScriptAssetReference::SetPath(std::string_view assetPath)
{
    path = util::FileSystem::NormalizePathSeparators(std::string(assetPath));
    std::string guidPath = path;
    std::string spriteTexturePath;
    std::string spriteToken;
    if (asset::ParseSpriteReference(path, spriteTexturePath, spriteToken))
        guidPath = spriteTexturePath;
    const std::string absolutePath = guidPath.empty()
        ? std::string{}
        : asset::AssetManager::ResolveAssetPath(guidPath);
    guid = absolutePath.empty()
        ? std::string{}
        : asset::AssetDatabase::TryGetGuidFromPath(absolutePath);
}

void ScriptAssetReference::Clear()
{
    guid.clear();
    path.clear();
}

std::string ScriptAssetReference::ResolvePath() const
{
    if (!guid.empty()) {
        const std::string resolved = asset::DecodeGuidRef(
            std::string(asset::AssetDatabase::kGuidPrefix) + guid);
        if (!resolved.empty() && !asset::AssetDatabase::IsGuidRef(resolved)) {
            std::string spriteTexturePath;
            std::string spriteToken;
            if (asset::ParseSpriteReference(path, spriteTexturePath, spriteToken))
                return asset::MakeSpriteReference(resolved, spriteToken);
            return resolved;
        }
    }
    return path;
}

bool ScriptAssetReference::IsValid() const
{
    return !guid.empty() || !path.empty();
}

} // namespace fbzz::scene
