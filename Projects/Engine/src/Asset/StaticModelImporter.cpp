/// @file    StaticModelImporter.cpp
/// @brief   静的メッシュ (ボーンなし) のインポートパイプライン。
/// @author  Hasegawa Jin
/// @date    2026-05-28
///
/// aiProcess_PreTransformVertices でノード階層を事前にフラット化し、
/// JoinIdenticalVertices で重複頂点を除去することでドローコールを削減する。
/// スキンメッシュにこれらを適用するとボーン割り当てが壊れるため、別パスとして分離する。
#include "ModelImporterInternal.hpp"

namespace fbzz::asset {

std::unique_ptr<Model> ImportStaticModel(const aiScene* scene,
                                         float unitScale,
                                         renderer::ResourceManager& resources)
{
    auto model = std::make_unique<Model>();
    for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* src = scene->mMeshes[mi];

        std::vector<renderer::Vertex> vertices(src->mNumVertices);
        for (uint32_t i = 0; i < src->mNumVertices; ++i)
            vertices[i] = ImportVertex(src, i, unitScale);

        auto indices = ImportIndices(src);

        auto mesh = std::make_unique<renderer::Mesh>();
        mesh->vertexBuffer = resources.CreateVertexBuffer(
            vertices.data(), vertices.size() * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
        mesh->indexBuffer = resources.CreateIndexBuffer(
            indices.data(), static_cast<uint32_t>(indices.size()));
        mesh->vertexCount = static_cast<uint32_t>(vertices.size());
        mesh->indexCount  = static_cast<uint32_t>(indices.size());
        mesh->cpuVertices = vertices;
        mesh->cpuIndices  = indices;
        mesh->ComputeBounds();

        model->meshes.push_back(std::move(mesh));
        model->materials.push_back(ImportMaterial(scene, src, resources));
    }

    /// @note aiProcess_PreTransformVertices で階層は既に「ルート 1 個に全メッシュ」へ潰れており、
    ///       ImportModelNodes を呼んでも同じ結果になる。配置側 (SpawnModelAssetHierarchy) に
    ///       「nodes が空なら別処理」という分岐を持たせないため、そのまま 1 ノードとして表現する。
    /// @note DCC の階層を残したい場合は STATIC_ASSIMP_FLAGS から PreTransformVertices を外し、
    ///       ImportModelNodes に差し替える (頂点にベイクされた変換はノードの TRS へ移る)。
    BuildFlatModelNode(*model, {});
    return model;
}

} // namespace fbzz::asset
