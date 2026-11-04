/// @file    AssetPath.cpp
/// @brief   アセット解決の既存サービスへ軽量な利用境界を接続する。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#include <Engine/Asset/AssetPath.hpp>
#include <Engine/Asset/AssetManager.hpp>

namespace fbzz::asset {
std::string ResolveAssetPath(const std::string& path)
{
    return AssetManager::ResolveAssetPath(path);
}
}
