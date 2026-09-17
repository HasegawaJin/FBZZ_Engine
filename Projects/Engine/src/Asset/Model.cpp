/// @file    Model.cpp
/// @brief   Model / ModelAsset が持つメッシュの GPU バッファを、所有者と一緒に返す
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// デストラクタだけのために .cpp を作る。ヘッダーで書くと Model.hpp が ResourceManager.hpp を
/// 巻き込み、Model を読むだけの箇所 (インポーター・シリアライザ) までレンダラーへ依存させて
/// しまう。宣言だけをヘッダーに置き、実体はここへ閉じる。
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
    /// @note Active() が空でも黙って通す。静的キャッシュ (AssetStore) はマネージャーより後に
    ///       畳まれることがあり、そこで «返せなかった» のはプロセスが終わる場面なので、
    ///       ログを出しても直せる相手が居ない。
    renderer::ResourceManager* resources = renderer::ResourceManager::Active();
    if (!resources) return;

    for (const std::unique_ptr<renderer::Mesh>& mesh : meshes)
        if (mesh) (void)resources->ReleaseMeshBuffers(*mesh);
}

} // namespace fbzz::asset
