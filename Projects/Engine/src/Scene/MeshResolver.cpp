/// @file    MeshResolver.cpp
/// @brief   メッシュ参照文字列の解決
/// @author  Hasegawa Jin
/// @date    2026-08-26
#include <Engine/Scene/MeshResolver.hpp>

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>

#include <cctype>

namespace fbzz::scene {

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

    std::string filePath  = path;
    int         meshIndex = 0;

    // Windows のドライブ文字を誤判定しないように、最後の '/' より後ろの ':' を探す
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

    auto* model = asset::AssetManager::LoadAndGet<asset::Model>(filePath);
    if (!model) return nullptr;
    if (meshIndex < 0 || meshIndex >= static_cast<int>(model->meshes.size())) return nullptr;
    return model->meshes[meshIndex].get();
}

} // namespace fbzz::scene
