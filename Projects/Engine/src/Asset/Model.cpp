/// @file    Model.cpp
/// @brief   Model / ModelAsset が持つメッシュの GPU バッファを、所有者と一緒に返す
/// @author  Hasegawa Jin
/// @date    2026-09-01

/// @note デストラクタだけのために .cpp を作る。ヘッダーで書くと Model.hpp が ResourceManager.hpp を
/// @note 巻き込み、Model を読むだけの箇所 (インポーター・シリアライザ) までレンダラーへ依存させて
/// @note しまう。宣言だけをヘッダーに置き、実体はここへ閉じる。
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

namespace fbzz::asset {
namespace {
template<typename Tag>
void ReleaseOwned(renderer::ResourceHandle<Tag>& handle)
{
    if (auto* owner = renderer::ResourceManager::FindOwner(handle.owner)) owner->Release(handle);
    handle = {};
}
void ReleaseOwnedMesh(renderer::Mesh& mesh)
{
    ReleaseOwned(mesh.vertexBuffer);
    ReleaseOwned(mesh.indexBuffer);
}
} /// @note namespace

ModelAsset::~ModelAsset()
{
    for (LodLevel& lod : lods)
        for (SubmeshEntry& submesh : lod.submeshes)
            if (submesh.mesh) ReleaseOwnedMesh(*submesh.mesh);
}

Model::~Model()
{
    for (const auto& mesh : meshes)
        if (mesh) ReleaseOwnedMesh(*mesh);
    ReleaseOwned(referencePoseCB);
}
} /// @note namespace fbzz::asset
