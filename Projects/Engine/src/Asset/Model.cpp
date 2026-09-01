/// @file    Model.cpp
/// @brief   Model / ModelAsset が持つメッシュの GPU バッファを、所有者と一緒に返す
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// WHY デストラクタだけのために .cpp を作るか:
///   ヘッダーで書くと Model.hpp が ResourceManager.hpp を巻き込む。あちらは
///   レンダラーの実装側の入口で、Model を読むだけの箇所 (インポーター・シリアライザ) まで
///   レンダラーへ依存させたくない。宣言だけをヘッダーに置き、実体はここへ閉じる。
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

namespace fbzz::asset {

ModelAsset::~ModelAsset()
{
    renderer::ResourceManager* resources = renderer::ResourceManager::Active();
    if (!resources) return;

    for (LodLevel& lod : lods)
        for (SubmeshEntry& submesh : lod.submeshes)
            if (submesh.mesh) (void)resources->ReleaseMeshBuffers(*submesh.mesh);
}

Model::~Model()
{
    // WHY Active() が空でも黙って通すか: 静的キャッシュ (AssetStore) はマネージャーより
    //     後に畳まれることがある。そこで «返せなかった» のはプロセスが終わる場面なので、
    //     ログを出しても直せる相手が居ない。
    renderer::ResourceManager* resources = renderer::ResourceManager::Active();
    if (!resources) return;

    for (const std::unique_ptr<renderer::Mesh>& mesh : meshes)
        if (mesh) (void)resources->ReleaseMeshBuffers(*mesh);
}

} // namespace fbzz::asset
