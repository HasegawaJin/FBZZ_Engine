// FBZZ Engine
// ModelImporter.cpp | fbzz::asset
// Assimp を使って FBX / OBJ 等を Model に変換する
#include <engine/Asset/ModelImporter.hpp>
#include <engine/Asset/Model.hpp>
#include <engine/Renderer/IRenderer.hpp>
#include <engine/Renderer/Mesh.hpp>
#include <engine/Renderer/Material.hpp>
#include <engine/Core/Logger.hpp>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <cassert>

namespace fbzz::asset {

namespace {

constexpr unsigned int ASSIMP_FLAGS =
    aiProcess_Triangulate           // ポリゴンを三角形に分割
  | aiProcess_JoinIdenticalVertices // 重複頂点をマージ
  | aiProcess_GenNormals            // 法線がなければ面法線を生成
  | aiProcess_MakeLeftHanded        // 右手系 → 左手系 (Z 反転)
  | aiProcess_FlipWindingOrder      // CCW → CW (MakeLeftHanded とセット)
  | aiProcess_FlipUVs               // V 反転 (OpenGL → DX11)
  | aiProcess_PreTransformVertices; // ノード変換を頂点にベイク

} // anonymous namespace

std::shared_ptr<Model> ModelImporter::Import(
    const std::string& path,
    renderer::IRenderer& renderer)
{
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path, ASSIMP_FLAGS);

    if (!scene || scene->mNumMeshes == 0) {
        FBZZ_LOG_ERROR("ModelImporter: %s の読み込み失敗", path.c_str());
        return nullptr;
    }

    // FBX の単位を m に変換。UnitScaleFactor は "1単位 = X cm" を表す。
    // cm モデル(UnitScaleFactor=1) → 0.01、m モデル(UnitScaleFactor=100) → 1.0
    // メタデータがない場合は FBX の慣例 (cm) を仮定して 0.01 にフォールバック
    float unitScale = 0.01f;
    if (scene->mMetaData) {
        double factorD = 1.0;
        float  factorF = 1.0f;
        if (scene->mMetaData->Get("UnitScaleFactor", factorD))
            unitScale = static_cast<float>(factorD) * 0.01f;
        else if (scene->mMetaData->Get("UnitScaleFactor", factorF))
            unitScale = factorF * 0.01f;
    }
    FBZZ_LOG_INFO("ModelImporter: %s  unitScale=%.4f", path.c_str(), unitScale);

    auto model = std::make_shared<Model>();

    for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* aim = scene->mMeshes[mi];

        std::vector<renderer::Vertex> vertices(aim->mNumVertices);
        for (uint32_t i = 0; i < aim->mNumVertices; ++i) {
            vertices[i].position = { aim->mVertices[i].x * unitScale,
                                     aim->mVertices[i].y * unitScale,
                                     aim->mVertices[i].z * unitScale };
            vertices[i].normal   = aim->mNormals
                ? math::Vector3{ aim->mNormals[i].x, aim->mNormals[i].y, aim->mNormals[i].z }
                : math::Vector3{ 0.0f, 1.0f, 0.0f };
            vertices[i].tangent  = aim->mTangents
                ? math::Vector3{ aim->mTangents[i].x, aim->mTangents[i].y, aim->mTangents[i].z }
                : math::Vector3{ 1.0f, 0.0f, 0.0f };
            if (aim->mTextureCoords[0])
                vertices[i].uv = { aim->mTextureCoords[0][i].x,
                                   aim->mTextureCoords[0][i].y };
        }

        std::vector<uint32_t> indices;
        indices.reserve(static_cast<size_t>(aim->mNumFaces) * 3);
        for (uint32_t fi = 0; fi < aim->mNumFaces; ++fi) {
            const aiFace& f = aim->mFaces[fi];
            indices.push_back(f.mIndices[0]);
            indices.push_back(f.mIndices[1]);
            indices.push_back(f.mIndices[2]);
        }

        auto mesh = std::make_shared<renderer::Mesh>();
        mesh->vertexBuffer = renderer.CreateVertexBuffer(
            vertices.data(),
            vertices.size() * sizeof(renderer::Vertex),
            sizeof(renderer::Vertex));
        mesh->indexBuffer  = renderer.CreateIndexBuffer(
            indices.data(), static_cast<uint32_t>(indices.size()));
        mesh->vertexCount  = static_cast<uint32_t>(vertices.size());
        mesh->indexCount   = static_cast<uint32_t>(indices.size());

        auto mat = std::make_shared<renderer::Material>();
        if (aim->mMaterialIndex < scene->mNumMaterials) {
            aiColor4D color(1.0f, 1.0f, 1.0f, 1.0f);
            scene->mMaterials[aim->mMaterialIndex]->Get(
                AI_MATKEY_COLOR_DIFFUSE, color);
            mat->params.albedo = { color.r, color.g, color.b, color.a };
        }
        mat->Init(renderer);

        model->meshes.push_back(mesh);
        model->materials.push_back(mat);
    }

    return model;
}

} // namespace fbzz::asset
