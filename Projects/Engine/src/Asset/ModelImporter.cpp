// FBZZ Engine
// ModelImporter.cpp | fbzz::asset
// 外部モデルファイルの読み込みエントリーポイント。
// スキニングの有無を判定し、静的・スキンメッシュのどちらのパスへ委譲するかを決める。
#include <Engine/Asset/ModelImporter.hpp>
#include <Engine/Core/Logger.hpp>
#include "ModelImporterInternal.hpp"
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>

namespace fbzz::asset {

namespace {

// 静的メッシュ用: PreTransformVertices でノード階層をフラット化し、
//                 JoinIdenticalVertices で重複頂点を除去する。
constexpr unsigned int STATIC_ASSIMP_FLAGS =
    aiProcess_Triangulate
  | aiProcess_JoinIdenticalVertices
  | aiProcess_GenNormals
  | aiProcess_MakeLeftHanded
  | aiProcess_FlipWindingOrder
  | aiProcess_FlipUVs
  | aiProcess_PreTransformVertices;

// スキンメッシュ用: PreTransformVertices / JoinIdenticalVertices を除外し
//                  ボーン割り当てとノード階層を保持する。
constexpr unsigned int SKINNED_ASSIMP_FLAGS =
    aiProcess_Triangulate
  | aiProcess_GenNormals
  | aiProcess_MakeLeftHanded
  | aiProcess_FlipWindingOrder
  | aiProcess_FlipUVs;

} // namespace

std::shared_ptr<Model> ModelImporter::Import(
    const std::string& path,
    renderer::ResourceManager& resources)
{
    // WHY: スキニング判定のためにまず SKINNED_FLAGS で読み込む (プローブ)。
    //      静的メッシュの最終インポートには STATIC_FLAGS が必要なため、判定後に再読み込みする。
    Assimp::Importer probeImporter;
    const aiScene* probeScene = probeImporter.ReadFile(path, SKINNED_ASSIMP_FLAGS);
    if (!probeScene) {
        FBZZ_LOG_ERROR("ModelImporter: failed to load %s — %s",
                       path.c_str(), probeImporter.GetErrorString());
        return nullptr;
    }
    if (probeScene->mNumMeshes == 0 && probeScene->mNumAnimations == 0) {
        FBZZ_LOG_ERROR("ModelImporter: %s has no meshes and no animations", path.c_str());
        return nullptr;
    }

    const bool  skinned       = HasSkinning(probeScene);
    const float probeUnitScale = ReadUnitScale(probeScene);

    if (skinned)
        return ImportSkinnedModel(probeScene, probeUnitScale, resources);

    // 静的メッシュとして、頂点結合フラグ付きで再インポートする
    Assimp::Importer staticImporter;
    const aiScene* staticScene = staticImporter.ReadFile(path, STATIC_ASSIMP_FLAGS);
    if (!staticScene || staticScene->mNumMeshes == 0) {
        FBZZ_LOG_ERROR("ModelImporter: failed to load static model %s", path.c_str());
        return nullptr;
    }

    return ImportStaticModel(staticScene, ReadUnitScale(staticScene), resources);
}

} // namespace fbzz::asset
