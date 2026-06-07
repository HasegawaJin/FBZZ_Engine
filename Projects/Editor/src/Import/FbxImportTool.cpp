// FBZZ Engine
// FbxImportTool.cpp | fbzz::editor
// FBX → fz* 変換パイプラインのオーケストレーター
#include <Editor/Import/FbxImportTool.hpp>
#include <Editor/Import/FzAnimationExporter.hpp>
#include <Editor/Import/FzAssetWriter.hpp>
#include <Editor/Import/FzMaterialExporter.hpp>
#include <Editor/Import/FzMeshExporter.hpp>
#include <Editor/Import/FzSkeletonExporter.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <assimp/config.h>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace fbzz::editor {

namespace {

// Assimp の読み込みフラグ
// WHY: MakeLeftHanded + FlipWindingOrder は既存の ModelImporter と同じ設定。
//      エンジンは DirectX 左手系のため右手系 FBX をここで変換する。
//      FlipUVs は DirectX の UV 原点 (左上) に合わせるため必要。
constexpr unsigned int kImportFlags =
    aiProcess_Triangulate            |
    aiProcess_GenSmoothNormals       |
    aiProcess_CalcTangentSpace       |
    aiProcess_JoinIdenticalVertices  |
    aiProcess_LimitBoneWeights       |
    aiProcess_ImproveCacheLocality   |
    aiProcess_MakeLeftHanded         |
    aiProcess_FlipWindingOrder       |
    aiProcess_FlipUVs;

// FBX メタデータから unitScale (メートル換算係数) を取得する。
// Assimp が "UnitScaleFactor" を持っていれば採用し、なければ 0.01 (cm→m) をデフォルトとする。
float ReadUnitScale(const aiScene* scene)
{
    if (!scene->mMetaData) return 0.01f;
    double factor = 1.0;
    if (scene->mMetaData->Get("UnitScaleFactor", factor))
        return static_cast<float>(factor * 0.01); // Assimp は cm 単位で返す
    return 0.01f;
}

// シーン内のいずれかのメッシュにボーンがあれば true
bool HasSkinning(const aiScene* scene)
{
    for (uint32_t i = 0; i < scene->mNumMeshes; ++i)
        if (scene->mMeshes[i]->HasBones()) return true;
    return false;
}

// マテリアルインデックス → 出力済みパスのキャッシュ。
// 同一マテリアルを複数メッシュが参照する場合にコピーを避ける。
struct MaterialCache {
    std::vector<std::string> paths; // インデックス対応、未処理は空文字
    explicit MaterialCache(uint32_t n) : paths(n) {}
};

} // namespace

bool FbxImportTool::Import(const std::string& fbxPath,
                            const std::string& outputDir,
                            const std::string& sourceHint)
{
    FBZZ_LOG_INFO("FbxImportTool: begin [%s] → [%s]", fbxPath.c_str(), outputDir.c_str());

    // ── Assimp でパース ───────────────────────────────────────────────────
    Assimp::Importer importer;
    // WHY: false にすることで FBX の Pivot/PreRotation 補助ノード ($AssimpFbx$_xxx) を
    //      通常ノードのローカル変換に畳み込む。ModelImporter と同じ設定にしないと
    //      メッシュ FBX とモーション専用 FBX でノード名・階層がずれてアニメーションが壊れる。
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* scene = importer.ReadFile(fbxPath, kImportFlags);
    if (!scene || !scene->mRootNode) {
        FBZZ_LOG_ERROR("FbxImportTool: Assimp parse failed [%s]: %s",
                        fbxPath.c_str(), importer.GetErrorString());
        return false;
    }
    if (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) {
        FBZZ_LOG_WARN("FbxImportTool: AI_SCENE_FLAGS_INCOMPLETE set [%s] — continuing anyway",
                       fbxPath.c_str());
    }
    FBZZ_LOG_INFO("FbxImportTool: Assimp OK — %u meshes, %u anims, %u mats (flags=0x%X)",
                  scene->mNumMeshes, scene->mNumAnimations, scene->mNumMaterials,
                  scene->mFlags);
    if (scene->mNumAnimations == 0) {
        FBZZ_LOG_WARN("FbxImportTool: no animation data in [%s] — manifest will have empty animations",
                       fbxPath.c_str());
    }

    const fs::path fbxFsPath = util::FileSystem::PathFromUtf8(fbxPath);
    const std::string fbxDir   = util::FileSystem::PathToUtf8(fbxFsPath.parent_path());
    const std::string baseName = util::FileSystem::PathToUtf8(fbxFsPath.stem());
    const float       unitScale = ReadUnitScale(scene);
    const bool        hasSkin   = HasSkinning(scene);

    // ── 出力ディレクトリを作成 ────────────────────────────────────────────
    const fs::path outDirPath = util::FileSystem::PathFromUtf8(outputDir);
    const fs::path meshDir  = outDirPath / "meshes";
    const fs::path matDir   = outDirPath / "materials";
    const fs::path texDir   = outDirPath / "textures";
    if (!util::FileSystem::EnsureDirectory(meshDir) ||
        !util::FileSystem::EnsureDirectory(matDir) ||
        !util::FileSystem::EnsureDirectory(texDir)) {
        FBZZ_LOG_ERROR("FbxImportTool: cannot create output dirs [%s]", outputDir.c_str());
        return false;
    }

    // 失敗時に outputDir 全体をロールバックするためのガード
    bool success = false;
    auto cleanup = [&] {
        if (!success) {
            util::FileSystem::RemoveAll(outDirPath);
        }
    };
    struct Guard { std::function<void()> fn; ~Guard() { fn(); } } guard{ cleanup };

    // ── マテリアル ────────────────────────────────────────────────────────
    MaterialCache matCache(scene->mNumMaterials);
    FzAssetManifest manifest;
    manifest.unitScale  = unitScale;
    manifest.sourceHint = sourceHint.empty() ? fbxPath : sourceHint;

    // ── メッシュ ──────────────────────────────────────────────────────────
    for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* mesh     = scene->mMeshes[mi];
        const bool    skinned  = mesh->HasBones();
        const std::string meshPath =
            util::FileSystem::PathToUtf8(meshDir / ("mesh_" + std::to_string(mi) + ".fzmesh"));

        if (!FzMeshExporter::Export(mesh, scene, unitScale, skinned, meshPath)) {
            FBZZ_LOG_ERROR("FbxImportTool: mesh export failed [%u]", mi);
            return false;
        }

        // .fzasset からの相対パス (manifest と同じ outputDir を起点にする)
        const std::string relMesh = util::FileSystem::PathToUtf8(
            util::FileSystem::RelativePath(util::FileSystem::PathFromUtf8(meshPath), outDirPath));
        manifest.meshPaths.push_back(relMesh);

        // 対応マテリアル
        const uint32_t matIdx = mesh->mMaterialIndex;
        if (matIdx < scene->mNumMaterials && matCache.paths[matIdx].empty()) {
            const std::string matPath =
                util::FileSystem::PathToUtf8(matDir / ("mat_" + std::to_string(matIdx) + ".fzmat"));
            if (!FzMaterialExporter::Export(scene->mMaterials[matIdx], scene,
                                             fbxDir, util::FileSystem::PathToUtf8(texDir), matPath)) {
                FBZZ_LOG_ERROR("FbxImportTool: material export failed [%u]", matIdx);
                return false;
            }
            matCache.paths[matIdx] = util::FileSystem::PathToUtf8(
                util::FileSystem::RelativePath(util::FileSystem::PathFromUtf8(matPath), outDirPath));
        }
        manifest.materialPaths.push_back(
            matIdx < scene->mNumMaterials ? matCache.paths[matIdx] : std::string{});
    }

    // ── スケルトン ────────────────────────────────────────────────────────
    if (hasSkin) {
        const std::string skelPath = util::FileSystem::PathToUtf8(outDirPath / (baseName + ".fzskel"));
        if (!FzSkeletonExporter::Export(scene, unitScale, skelPath)) {
            FBZZ_LOG_ERROR("FbxImportTool: skeleton export failed");
            return false;
        }
        manifest.skeletonPath = util::FileSystem::PathToUtf8(
            util::FileSystem::RelativePath(util::FileSystem::PathFromUtf8(skelPath), outDirPath));
    }

    // ── アニメーション ────────────────────────────────────────────────────
    const fs::path animDir = outDirPath / "anims";
    if (scene->mNumAnimations > 0) {
        util::FileSystem::EnsureDirectory(animDir);
        for (uint32_t ai = 0; ai < scene->mNumAnimations; ++ai) {
            const std::string animPath =
                util::FileSystem::PathToUtf8(animDir / ("clip_" + std::to_string(ai) + ".fzanim"));
            if (!FzAnimationExporter::Export(scene->mAnimations[ai], animPath, unitScale)) {
                FBZZ_LOG_ERROR("FbxImportTool: animation export failed [%u]", ai);
                return false;
            }
            manifest.animPaths.push_back(util::FileSystem::PathToUtf8(
                util::FileSystem::RelativePath(util::FileSystem::PathFromUtf8(animPath), outDirPath)));
        }
    }

    // ── マニフェスト ──────────────────────────────────────────────────────
    const std::string manifestPath =
        util::FileSystem::PathToUtf8(outDirPath / (baseName + ".fzasset"));
    if (!FzAssetWriter::Write(manifest, manifestPath)) {
        FBZZ_LOG_ERROR("FbxImportTool: manifest write failed");
        return false;
    }

    FBZZ_LOG_INFO("FbxImportTool: done → [%s]", manifestPath.c_str());
    success = true;
    return true;
}

} // namespace fbzz::editor
