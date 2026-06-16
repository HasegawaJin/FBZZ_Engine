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
constexpr unsigned int kBaseImportFlags =
    aiProcess_Triangulate            |
    aiProcess_GenSmoothNormals       |
    aiProcess_CalcTangentSpace       |
    aiProcess_JoinIdenticalVertices  |
    aiProcess_LimitBoneWeights       |
    aiProcess_ImproveCacheLocality   |
    aiProcess_MakeLeftHanded         |
    aiProcess_FlipWindingOrder       |
    aiProcess_FlipUVs;

// WHY: 静的メッシュはノード階層のルート回転 (座標系補正) やスケールが
//      ノード変換に埋め込まれている場合がある。PreTransformVertices で
//      それらを頂点座標にベイクしないと、ModelImporter (FBX 直接ロード) と
//      fzasset 経由のロードでポーズが 90° ずれたりスケールが 1/100 になる。
//      スキンメッシュには適用しない (ボーン割り当てが壊れるため)。
constexpr unsigned int kStaticImportFlags =
    kBaseImportFlags | aiProcess_PreTransformVertices;

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
                            const std::string& sourceHint,
                            const FbxImportOptions& options)
{
    FBZZ_LOG_INFO("FbxImportTool: begin [%s] → [%s]", fbxPath.c_str(), outputDir.c_str());

    // ── Assimp でパース (第 1 パス: スキニング検出 + アニメーション用) ─────
    // WHY: false にすることで FBX の Pivot/PreRotation 補助ノード ($AssimpFbx$_xxx) を
    //      通常ノードのローカル変換に畳み込む。ModelImporter と同じ設定にしないと
    //      メッシュ FBX とモーション専用 FBX でノード名・階層がずれてアニメーションが壊れる。
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* scene = importer.ReadFile(fbxPath, kBaseImportFlags);
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
    const bool        hasSkin  = HasSkinning(scene);

    // ── 静的メッシュ用: 第 2 パス (PreTransformVertices でノード変換をベイク) ─
    // WHY: スキンメッシュはボーン階層が必要なため PreTransformVertices を使わない。
    //      静的メッシュのみ再ロードし、ノード回転・スケールを頂点座標に焼き込む。
    Assimp::Importer staticImporter;
    const aiScene* meshScene = scene;
    if (!hasSkin) {
        staticImporter.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        meshScene = staticImporter.ReadFile(fbxPath, kStaticImportFlags);
        if (!meshScene || !meshScene->mRootNode) {
            FBZZ_LOG_ERROR("FbxImportTool: static reimport failed [%s]: %s",
                            fbxPath.c_str(), staticImporter.GetErrorString());
            return false;
        }
    }

    const float unitScale = ReadUnitScale(meshScene);

    // ── 出力ディレクトリを作成 ────────────────────────────────────────────
    const fs::path outDirPath = util::FileSystem::PathFromUtf8(outputDir);
    // WHY: マニフェスト (.fzasset) は FBX と同じ階層 (outDirPath の親) に置く。
    //      AssetBrowser が Unity スタイルでフラット表示できるようにするため。
    //      メッシュ/マテリアルデータは outDirPath (stem/) サブフォルダに隔離したまま。
    const fs::path manifestDirPath = outDirPath.parent_path();
    const fs::path meshDir  = outDirPath / "meshes";
    const fs::path matDir   = outDirPath / "materials";
    const fs::path texDir   = outDirPath / "textures";
    if (!util::FileSystem::EnsureDirectory(meshDir) ||
        !util::FileSystem::EnsureDirectory(matDir) ||
        !util::FileSystem::EnsureDirectory(texDir)) {
        FBZZ_LOG_ERROR("FbxImportTool: cannot create output dirs [%s]", outputDir.c_str());
        return false;
    }

    // 失敗時にデータフォルダとマニフェストをロールバックするためのガード
    bool success = false;
    std::string manifestPath; // cleanup から参照するため早期宣言
    auto cleanup = [&] {
        if (!success) {
            util::FileSystem::RemoveAll(outDirPath);
            if (!manifestPath.empty()) {
                std::error_code ec;
                std::filesystem::remove(util::FileSystem::PathFromUtf8(manifestPath), ec);
            }
        }
    };
    struct Guard { std::function<void()> fn; ~Guard() { fn(); } } guard{ cleanup };

    // ── マテリアル ────────────────────────────────────────────────────────
    MaterialCache matCache(meshScene->mNumMaterials);
    FzAssetManifest manifest;
    manifest.unitScale  = unitScale;
    manifest.sourceHint = sourceHint.empty() ? fbxPath : sourceHint;

    // ── メッシュ ──────────────────────────────────────────────────────────
    for (uint32_t mi = 0; mi < meshScene->mNumMeshes; ++mi) {
        const aiMesh* mesh     = meshScene->mMeshes[mi];

        // 選択的インポート: selectedMeshNames が空でなければ一致するものだけ処理
        if (!options.selectedMeshNames.empty()) {
            const std::string meshName = mesh->mName.C_Str();
            bool selected = false;
            for (const auto& n : options.selectedMeshNames)
                if (n == meshName) { selected = true; break; }
            if (!selected) continue;
        }

        const bool    skinned  = mesh->HasBones();
        const std::string meshPath =
            util::FileSystem::PathToUtf8(meshDir / ("mesh_" + std::to_string(mi) + ".fzmesh"));

        if (!FzMeshExporter::Export(mesh, meshScene, unitScale, skinned, meshPath)) {
            FBZZ_LOG_ERROR("FbxImportTool: mesh export failed [%u]", mi);
            return false;
        }

        // .fzasset からの相対パス (manifest は manifestDirPath に置くので起点もそこ)
        const std::string relMesh = util::FileSystem::PathToUtf8(
            util::FileSystem::RelativePath(util::FileSystem::PathFromUtf8(meshPath), manifestDirPath));
        manifest.meshPaths.push_back(relMesh);

        // 対応マテリアル
        const uint32_t matIdx = mesh->mMaterialIndex;
        if (matIdx < meshScene->mNumMaterials && matCache.paths[matIdx].empty()) {
            const std::string matPath =
                util::FileSystem::PathToUtf8(matDir / ("mat_" + std::to_string(matIdx) + ".fzmat"));
            if (!FzMaterialExporter::Export(meshScene->mMaterials[matIdx], meshScene,
                                             fbxDir, util::FileSystem::PathToUtf8(texDir), matPath, skinned,
                                             options.flipGreenChannel)) {
                FBZZ_LOG_ERROR("FbxImportTool: material export failed [%u]", matIdx);
                return false;
            }
            matCache.paths[matIdx] = util::FileSystem::PathToUtf8(
                util::FileSystem::RelativePath(util::FileSystem::PathFromUtf8(matPath), manifestDirPath));
        }
        manifest.materialPaths.push_back(
            matIdx < meshScene->mNumMaterials ? matCache.paths[matIdx] : std::string{});
    }

    // ── スケルトン ────────────────────────────────────────────────────────
    if (hasSkin) {
        const std::string skelPath = util::FileSystem::PathToUtf8(outDirPath / (baseName + ".fzskel"));
        if (!FzSkeletonExporter::Export(scene, unitScale, skelPath)) {
            FBZZ_LOG_ERROR("FbxImportTool: skeleton export failed");
            return false;
        }
        manifest.skeletonPath = util::FileSystem::PathToUtf8(
            util::FileSystem::RelativePath(util::FileSystem::PathFromUtf8(skelPath), manifestDirPath));
    }

    // ── アニメーション ────────────────────────────────────────────────────
    const fs::path animDir = outDirPath / "anims";
    if (scene->mNumAnimations > 0) {
        util::FileSystem::EnsureDirectory(animDir);
        for (uint32_t ai = 0; ai < scene->mNumAnimations; ++ai) {
            // 選択的インポート: selectedAnimNames が空でなければ一致するものだけ処理
            if (!options.selectedAnimNames.empty()) {
                const std::string animName = scene->mAnimations[ai]->mName.C_Str();
                bool selected = false;
                for (const auto& n : options.selectedAnimNames)
                    if (n == animName) { selected = true; break; }
                if (!selected) continue;
            }

            const std::string animPath =
                util::FileSystem::PathToUtf8(animDir / ("clip_" + std::to_string(ai) + ".fzanim"));
            if (!FzAnimationExporter::Export(scene->mAnimations[ai], animPath, unitScale)) {
                FBZZ_LOG_ERROR("FbxImportTool: animation export failed [%u]", ai);
                return false;
            }
            manifest.animPaths.push_back(util::FileSystem::PathToUtf8(
                util::FileSystem::RelativePath(util::FileSystem::PathFromUtf8(animPath), manifestDirPath)));
        }
    }

    // ── マニフェスト ──────────────────────────────────────────────────────
    // manifestDirPath (= FBX と同じフォルダ) に stem.fzasset を書き出す
    manifestPath = util::FileSystem::PathToUtf8(manifestDirPath / (baseName + ".asset"));
    if (!FzAssetWriter::Write(manifest, manifestPath)) {
        FBZZ_LOG_ERROR("FbxImportTool: manifest write failed");
        return false;
    }

    FBZZ_LOG_INFO("FbxImportTool: done → [%s]", manifestPath.c_str());
    success = true;
    return true;
}

FbxScanResult FbxImportTool::Scan(const std::string& fbxPath)
{
    FbxScanResult result;
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    // aiProcess_Triangulate だけで十分。名前列挙のみなので重い後処理フラグは省く。
    const aiScene* scene = importer.ReadFile(fbxPath, aiProcess_Triangulate);
    if (!scene || !scene->mRootNode) return result;

    for (uint32_t i = 0; i < scene->mNumMeshes; ++i)
        result.meshNames.emplace_back(scene->mMeshes[i]->mName.C_Str());
    for (uint32_t i = 0; i < scene->mNumAnimations; ++i)
        result.animNames.emplace_back(scene->mAnimations[i]->mName.C_Str());
    result.valid = true;
    return result;
}

} // namespace fbzz::editor
