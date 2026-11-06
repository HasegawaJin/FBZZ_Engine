/// @file    ModelImporter.cpp
/// @brief   外部モデルファイルの読み込みエントリーポイント。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include <Engine/Asset/ModelImporter.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include "ModelImporterInternal.hpp"
#include <assimp/config.h>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>

namespace fbzz::asset {

namespace {

/// @note 静的メッシュだけ階層のフラット化と頂点結合を許し、ボーン割り当てを保護する。
constexpr unsigned int STATIC_ASSIMP_FLAGS =
    aiProcess_Triangulate
  | aiProcess_JoinIdenticalVertices
  | aiProcess_GenNormals
  | aiProcess_MakeLeftHanded
  | aiProcess_FlipWindingOrder
  | aiProcess_FlipUVs
  | aiProcess_PreTransformVertices;

constexpr unsigned int SKINNED_ASSIMP_FLAGS =
    aiProcess_Triangulate
  | aiProcess_GenNormals
  | aiProcess_MakeLeftHanded
  | aiProcess_FlipWindingOrder
  | aiProcess_FlipUVs;

void ConfigureFbxImporter(Assimp::Importer& importer)
{
    /// @note メッシュ FBX とモーション専用 FBX の骨名・階層を揃えるため、Pivot 補助ノードを変換へ畳み込む。
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
}

} /// @note namespace

std::unique_ptr<Model> ModelImporter::Import(
    const std::string& path,
    renderer::ResourceManager& resources)
{
    /// @note ベイク経路と揃え、OBJ の原本数値と glTF/GLB のメートル単位へ FBX の cm 換算を適用しない。
    /// @see https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#coordinate-system-and-units glTF 2.0 Coordinate System and Units
    const auto extension = util::StringUtils::ToLowerCopy(util::FileSystem::GetExtension(path));
    const bool preserveSourceUnits = extension == ".obj" || extension == ".gltf" || extension == ".glb";
    /// @note 静的メッシュの頂点結合でスキンを壊さないよう、骨の有無を先に調べる。
    Assimp::Importer probeImporter;
    ConfigureFbxImporter(probeImporter);
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
    const float probeUnitScale = preserveSourceUnits ? 1.0f : ReadUnitScale(probeScene);

    if (skinned)
        return ImportSkinnedModel(probeScene, probeUnitScale, resources);

    Assimp::Importer staticImporter;
    ConfigureFbxImporter(staticImporter);
    const aiScene* staticScene = staticImporter.ReadFile(path, STATIC_ASSIMP_FLAGS);
    if (!staticScene || staticScene->mNumMeshes == 0) {
        FBZZ_LOG_ERROR("ModelImporter: failed to load static model %s", path.c_str());
        return nullptr;
    }

    return ImportStaticModel(staticScene, preserveSourceUnits ? 1.0f : ReadUnitScale(staticScene), resources);
}

} /// @note namespace fbzz::asset
