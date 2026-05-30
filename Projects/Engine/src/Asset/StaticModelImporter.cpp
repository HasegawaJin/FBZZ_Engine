// FBZZ Engine
// StaticModelImporter.cpp | fbzz::asset
// 静的メッシュ (ボーンなし) のインポートパイプライン。
// WHY: aiProcess_PreTransformVertices でノード階層を事前にフラット化し、
//      JoinIdenticalVertices で重複頂点を除去することでドローコールを削減する。
//      スキンメッシュにこれらを適用するとボーン割り当てが壊れるため、別パスとして分離する。
#include "ModelImporterInternal.hpp"

namespace fbzz::asset {

std::shared_ptr<Model> ImportStaticModel(const aiScene* scene,
                                         float unitScale,
                                         renderer::ResourceManager& resources)
{
    auto model = std::make_shared<Model>();
    for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* src = scene->mMeshes[mi];

        std::vector<renderer::Vertex> vertices(src->mNumVertices);
        for (uint32_t i = 0; i < src->mNumVertices; ++i)
            vertices[i] = ImportVertex(src, i, unitScale);

        auto indices = ImportIndices(src);

        auto mesh = std::make_shared<renderer::Mesh>();
        mesh->vertexBuffer = resources.CreateVertexBuffer(
            vertices.data(), vertices.size() * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
        mesh->indexBuffer = resources.CreateIndexBuffer(
            indices.data(), static_cast<uint32_t>(indices.size()));
        mesh->vertexCount = static_cast<uint32_t>(vertices.size());
        mesh->indexCount  = static_cast<uint32_t>(indices.size());
        mesh->cpuVertices = vertices;
        mesh->cpuIndices  = indices;
        mesh->ComputeBounds();

        model->meshes.push_back(mesh);
        model->materials.push_back(ImportMaterial(scene, src, resources));
    }
    return model;
}

} // namespace fbzz::asset
