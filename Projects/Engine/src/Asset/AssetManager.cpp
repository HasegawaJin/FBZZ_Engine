// FBZZ Engine
// AssetManager.cpp | fbzz::asset
// Model と Texture のロードおよびキャッシュ管理
// 相対パスを正規化し、同じアセットを重複ロードしない。
// Texture は ResourceManager、Model は ModelImporter を通して生成する。
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/ModelImporter.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Core/Logger.hpp>
#include <algorithm>
#include <cassert>

namespace fbzz::asset {

renderer::ResourceManager* AssetManager::s_resources = nullptr;
std::string AssetManager::s_basePath = "assets/";
bool AssetManager::s_initialized = false;
std::unordered_map<std::string, std::shared_ptr<Model>> AssetManager::s_models;
std::unordered_map<std::string, renderer::ResourceHandle<renderer::TextureTag>> AssetManager::s_textures;

std::string AssetManager::Normalize(const std::string& path)
{
    std::string result = path;
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

// basePath + key を優先して試し、存在しなければ key をそのまま使う
static std::string ResolvePath(const std::string& key, const std::string& basePath)
{
    const std::string full = basePath + key;
    if (util::FileSystem::Exists(full)) return full;
    if (util::FileSystem::Exists(key))  return key;
    return full; // 存在しなくても呼び出し元に任せる (エラーログは呼び出し元で)
}

void AssetManager::Init(renderer::ResourceManager& resources, const std::string& basePath)
{
    assert(!s_initialized && "AssetManager::Init() must be called once");
    s_resources = &resources;
    s_basePath = basePath;
    s_initialized = true;
}

void AssetManager::UnloadAll()
{
    s_models.clear();
    s_textures.clear();
    s_resources = nullptr;
    s_initialized = false;
}

renderer::ResourceHandle<renderer::TextureTag> AssetManager::LoadTexture(const std::string& relativePath)
{
    assert(s_initialized && "AssetManager::Init() must be called first");

    const std::string key = Normalize(relativePath);
    auto it = s_textures.find(key);
    if (it != s_textures.end()) return it->second;

    const std::string fullPath = ResolvePath(key, s_basePath);
    renderer::ResourceHandle<renderer::TextureTag> texture = s_resources->LoadTexture(fullPath);
    if (!texture.IsValid()) {
        FBZZ_LOG_ERROR("AssetManager: Texture load failed [%s]", fullPath.c_str());
        return {};
    }

    s_textures[key] = texture;
    return texture;
}

template<>
std::shared_ptr<Model> AssetManager::Load<Model>(const std::string& relativePath)
{
    assert(s_initialized && "AssetManager::Init() must be called first");

    const std::string key = Normalize(relativePath);
    auto it = s_models.find(key);
    if (it != s_models.end()) return it->second;

    const std::string fullPath = ResolvePath(key, s_basePath);
    auto model = ModelImporter::Import(fullPath, *s_resources);
    if (!model) {
        FBZZ_LOG_ERROR("AssetManager: Model load failed [%s]", fullPath.c_str());
        return nullptr;
    }

    s_models[key] = model;
    return model;
}

template<>
void AssetManager::Unload<Model>(const std::string& relativePath)
{
    s_models.erase(Normalize(relativePath));
}

} // namespace fbzz::asset
