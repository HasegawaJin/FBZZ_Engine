/// @file    AssetSession.hpp
/// @brief   ホストの資源所有者より先にアセットサービスを終了するスコープ。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#pragma once
#include <Engine/Asset/AssetManager.hpp>

namespace fbzz::asset {

/// @note ResourceManager の後、Scene とホストモジュールの前に宣言し、非同期候補を Manager の生存中に破棄する。
class AssetSession final {
public:
    explicit AssetSession(renderer::ResourceManager& resources, const std::string& basePath)
    {
        AssetManager::Init(resources, basePath);
    }
    ~AssetSession() { AssetManager::UnloadAll(); }
    AssetSession(const AssetSession&) = delete;
    AssetSession& operator=(const AssetSession&) = delete;
};

} /// @note namespace fbzz::asset
