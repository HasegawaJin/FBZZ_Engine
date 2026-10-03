/// @file    AssetPathService.cpp
/// @brief   アセットパス解決の差し込み口と、未登録時の素通し実装。
/// @author  Hasegawa Jin
/// @date    2026-09-03
#include <Graphics/Renderer/AssetPathService.hpp>

namespace fbzz::renderer {

namespace {

/// @note 登録側 (AssetManager.cpp) は静的初期化で呼ぶ。名前空間スコープの変数にすると
/// @note 翻訳単位間の初期化順が未規定で、登録済みの値を空で上書きしうる。
AssetPathService& Service()
{
    static AssetPathService service{};
    return service;
}

} /// @note namespace

void SetAssetPathService(const AssetPathService& service)
{
    Service() = service;
}

ShaderCapabilities ResolveShaderCapabilities(std::string_view reference)
{
    const auto& service = Service();
    return service.resolveShaderCapabilities ? service.resolveShaderCapabilities(reference) : ShaderCapabilities{};
}

std::string ResolveAssetPath(const std::string& path)
{
    const auto& service = Service();
    return service.resolveAssetPath ? service.resolveAssetPath(path) : path;
}

bool ResolveTextureSource(std::string_view texturePath, std::string& outSourcePath)
{
    const auto& service = Service();
    if (service.resolveTextureSource) {
        return service.resolveTextureSource(texturePath, outSourcePath);
    }
    outSourcePath.assign(texturePath);
    return true;
}

std::string NormalizeTextureKey(std::string_view reference)
{
    const auto& service = Service();
    std::string key;
    if (service.normalizeTextureKey) {
        service.normalizeTextureKey(reference, key);
        return key;
    }
    key.assign(reference);
    return key;
}

} /// @note namespace fbzz::renderer
