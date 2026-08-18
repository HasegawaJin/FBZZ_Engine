// FBZZ Engine
// StaticModelImporter.cpp | fbzz::asset
// 静的メッシュ (ボーンなし) のインポートパイプライン。
// WHY: aiProcess_PreTransformVertices でノード階層を事前にフラット化し、
//      JoinIdenticalVertices で重複頂点を除去することでドローコールを削減する。
//      スキンメッシュにこれらを適用するとボーン割り当てが壊れるため、別パスとして分離する。
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

    // 静的パスは aiProcess_PreTransformVertices を通しており、この時点でノード階層は
    // 既に潰れて「ルート 1 個に全メッシュ」の形になっている。ImportModelNodes を呼んでも
    // 同じ結果しか得られないため、その事実をそのまま 1 ノードとして表現する。
    //
    // WHY 平坦でもノードを作るか: 配置側 (SpawnModelAssetHierarchy) に
    //     「nodes が空なら別処理」という分岐を持たせないため。静的も スキンドも
    //     ノード木を辿る 1 本の経路で扱えるようにしておく。
    // NOTE: 静的モデルでも DCC の階層を残したい場合は STATIC_ASSIMP_FLAGS から
    //       PreTransformVertices を外し、ここを ImportModelNodes へ差し替えること。
    //       頂点にベイクされていた変換はノードの TRS として GameObject 側へ移る。
    BuildFlatModelNode(*model, {});
    return model;
}

} // namespace fbzz::asset
