/// @file    MeshResolver.cpp
/// @brief   メッシュ参照文字列の解決
/// @author  Hasegawa Jin
/// @date    2026-08-26
#include <Engine/Scene/MeshResolver.hpp>

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <unordered_map>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>

#include <cctype>

namespace fbzz::scene {
namespace {
/// @note GUID identity is stable until the index revision changes; handles are resolved again after unload/reimport.
struct GuidMeshResolutionCache {
    struct Entry { asset::AssetHandle<asset::Model> model; int meshIndex = 0; };
    uint64_t resourceOwner = 0, resourceEpoch = 0, indexRevision = 0;
    int assetGeneration = -1;
    std::unordered_map<std::string, Entry> entries;
    bool Matches(const renderer::ResourceManager& resources) const
    {
        return resourceOwner == resources.GetInstanceId() && resourceEpoch == resources.GetResetVersion()
            && assetGeneration == asset::AssetManager::GetAssetGeneration()
            && indexRevision == asset::AssetDatabase::Revision();
    }
    void Refresh(const renderer::ResourceManager& resources)
    {
        if (Matches(resources)) return;
        entries.clear(); resourceOwner = resources.GetInstanceId(); resourceEpoch = resources.GetResetVersion();
        assetGeneration = asset::AssetManager::GetAssetGeneration(); indexRevision = asset::AssetDatabase::Revision();
    }
};
thread_local GuidMeshResolutionCache s_guidMeshResolutionCache;
} /// @note namespace


renderer::Mesh* ResolveMeshPath(const std::string& path, renderer::ResourceManager& resources)
{
    if (path.empty()) return nullptr;

    if (path.starts_with("primitive:")) {
        if (path == "primitive:cube")     return renderer::PrimitiveMesh::Cube(resources);
        if (path == "primitive:sphere")   return renderer::PrimitiveMesh::Sphere(resources, 32);
        if (path == "primitive:plane")    return renderer::PrimitiveMesh::Plane(resources);
        if (path == "primitive:quad")     return renderer::PrimitiveMesh::Quad(resources);
        if (path == "primitive:cylinder") return renderer::PrimitiveMesh::Cylinder(resources);
        if (path == "primitive:cone")     return renderer::PrimitiveMesh::Cone(resources);
        if (path == "primitive:torus")    return renderer::PrimitiveMesh::Torus(resources);
        if (path == "primitive:capsule")  return renderer::PrimitiveMesh::Capsule(resources);
        return nullptr;
    }

    /// @note Relative/absolute paths retain their filesystem precedence checks. Failed GUID loads are never cached.
    const bool cacheGuid = asset::AssetDatabase::IsGuidRef(path);
    auto& cache = s_guidMeshResolutionCache;
    if (cacheGuid) {
        cache.Refresh(resources);
        const auto found = cache.entries.find(path);
        if (found != cache.entries.end()) {
            const auto entry = found->second;
            if (auto* model = asset::AssetManager::Get(entry.model)) {
                if (entry.meshIndex >= 0 && entry.meshIndex < static_cast<int>(model->meshes.size()))
                    return model->meshes[entry.meshIndex].get();
            }
            cache.entries.erase(found);
        }
    }

    std::string filePath  = path;
    int         meshIndex = 0;

    /// @note Windows のドライブ文字を誤判定しないように、最後の '/' より後ろの ':' を探す
    const size_t slashPos   = path.find_last_of('/');
    const size_t searchFrom = (slashPos != std::string::npos) ? slashPos : 0;
    const size_t colonPos   = path.find(':', searchFrom);

    if (colonPos != std::string::npos) {
        const std::string_view suffix(path.data() + colonPos + 1, path.size() - colonPos - 1);
        bool allDigits = !suffix.empty();
        for (char c : suffix) {
            if (!std::isdigit(static_cast<unsigned char>(c))) { allDigits = false; break; }
        }
        if (allDigits) {
            filePath = path.substr(0, colonPos);
            for (char c : suffix) meshIndex = meshIndex * 10 + (c - '0');
        }
    }

    const auto handle = asset::AssetManager::Load<asset::Model>(filePath);
    auto* model = asset::AssetManager::Get(handle);
    if (!model) return nullptr;
    if (meshIndex < 0 || meshIndex >= static_cast<int>(model->meshes.size())) return nullptr;
    /// @note Bound retained keys; resource callbacks may have changed the lookup epoch during Load.
    if (cacheGuid && cache.Matches(resources) && cache.entries.size() < 4096)
        cache.entries.insert_or_assign(path, GuidMeshResolutionCache::Entry{handle, meshIndex});
    return model->meshes[meshIndex].get();
}

} /// @note namespace fbzz::scene
