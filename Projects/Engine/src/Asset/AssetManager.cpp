// FBZZ Engine
// AssetManager.cpp | fbzz::asset
// Model / ITexture のロードとキャッシュ管理
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/ModelImporter.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Core/Logger.hpp>
#include <cassert>
#include <algorithm>

namespace fbzz::asset {

renderer::IRenderer*                                               AssetManager::s_renderer    = nullptr;
std::string                                                        AssetManager::s_basePath    = "assets/";
bool                                                               AssetManager::s_initialized = false;
std::unordered_map<std::string, std::shared_ptr<Model>>            AssetManager::s_models;
std::unordered_map<std::string, std::shared_ptr<renderer::ITexture>> AssetManager::s_textures;

// ------------------------------------------------------------- helpers

std::string AssetManager::Normalize(const std::string& path)
{
    std::string result = path;
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

// ------------------------------------------------------------- lifecycle

void AssetManager::Init(renderer::IRenderer& renderer, const std::string& basePath)
{
    assert(!s_initialized && "AssetManager::Init() は一度だけ呼ぶこと");
    s_renderer    = &renderer;
    s_basePath    = basePath;
    s_initialized = true;
}

void AssetManager::UnloadAll()
{
    s_models.clear();
    s_textures.clear();
    s_renderer    = nullptr;
    s_initialized = false;
}

// ------------------------------------------------------------- Load<Model>

template<>
std::shared_ptr<Model> AssetManager::Load<Model>(const std::string& relativePath)
{
    assert(s_initialized && "AssetManager::Init() を先に呼ぶこと");

    const std::string key = Normalize(relativePath);

    auto it = s_models.find(key);
    if (it != s_models.end())
        return it->second;

    const std::string fullPath = s_basePath + key;
    auto model = ModelImporter::Import(fullPath, *s_renderer);
    if (!model) {
        FBZZ_LOG_ERROR("AssetManager: Model ロード失敗 [%s]", fullPath.c_str());
        return nullptr;
    }

    s_models[key] = model;
    return model;
}

// ------------------------------------------------------------- Load<ITexture>

template<>
std::shared_ptr<renderer::ITexture> AssetManager::Load<renderer::ITexture>(
    const std::string& relativePath)
{
    assert(s_initialized && "AssetManager::Init() を先に呼ぶこと");

    const std::string key = Normalize(relativePath);

    auto it = s_textures.find(key);
    if (it != s_textures.end())
        return it->second;

    const std::string fullPath = s_basePath + key;
    auto tex = s_renderer->CreateTexture(fullPath);
    if (!tex) {
        FBZZ_LOG_ERROR("AssetManager: Texture ロード失敗 [%s]", fullPath.c_str());
        return nullptr;
    }

    s_textures[key] = tex;
    return tex;
}

// ------------------------------------------------------------- Unload<Model>

template<>
void AssetManager::Unload<Model>(const std::string& relativePath)
{
    s_models.erase(Normalize(relativePath));
}

// ------------------------------------------------------------- Unload<ITexture>

template<>
void AssetManager::Unload<renderer::ITexture>(const std::string& relativePath)
{
    s_textures.erase(Normalize(relativePath));
}

} // namespace fbzz::asset
