// FBZZ Engine
// FbxImportTool.cpp | fbzz::editor
// FBX → fz* 変換パイプラインのオーケストレーター (BuildPipeline パターン)
//
// 旧パイプライン: FbxImportTool → FzMeshExporter + FzSkeletonExporter + FzAnimationExporter
//                → FzAssetWriter (.asset マニフェスト)
// 新パイプライン: FbxImportTool → BuildPipeline() → IFbxSubExporter[]
//                → FzModelSubExporter (.model)
//                → FzAnimSubExporter  (.anim v2)
//                → FzMatSubExporter   (.mat + textures/ コピー)
//                → FzTexSubExporter   (.tex 自動生成、FzMatSubExporter の textures/ 出力が前提)
#include <Editor/Import/FbxImportTool.hpp>
#include <Editor/Import/FzAnimSubExporter.hpp>
#include <Editor/Import/FzMatSubExporter.hpp>
#include <Editor/Import/FzModelSubExporter.hpp>
#include <Editor/Import/FzTexSubExporter.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <assimp/config.h>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <filesystem>
#include <functional>

namespace fs = std::filesystem;

namespace fbzz::editor {

namespace {

constexpr unsigned int kBaseFlags =
    aiProcess_Triangulate           |
    aiProcess_GenSmoothNormals      |
    aiProcess_CalcTangentSpace      |
    aiProcess_JoinIdenticalVertices |
    aiProcess_LimitBoneWeights      |
    aiProcess_ImproveCacheLocality  |
    aiProcess_MakeLeftHanded        |
    aiProcess_FlipWindingOrder      |
    aiProcess_FlipUVs;

constexpr unsigned int kStaticFlags =
    kBaseFlags | aiProcess_PreTransformVertices;

float ReadUnitScale(const aiScene* scene)
{
    if (!scene->mMetaData) return 0.01f;
    double factor = 1.0;
    if (scene->mMetaData->Get("UnitScaleFactor", factor))
        return static_cast<float>(factor * 0.01);
    return 0.01f;
}

bool HasSkinning(const aiScene* scene)
{
    for (uint32_t i = 0; i < scene->mNumMeshes; ++i)
        if (scene->mMeshes[i]->HasBones()) return true;
    return false;
}

} // namespace

std::vector<std::unique_ptr<IFbxSubExporter>> FbxImportTool::BuildPipeline()
{
    std::vector<std::unique_ptr<IFbxSubExporter>> pipeline;
    pipeline.push_back(std::make_unique<FzModelSubExporter>());
    pipeline.push_back(std::make_unique<FzAnimSubExporter>());
    pipeline.push_back(std::make_unique<FzMatSubExporter>());
    pipeline.push_back(std::make_unique<FzTexSubExporter>());
    return pipeline;
}

bool FbxImportTool::Import(const std::string& fbxPath,
                            const std::string& outputDir,
                            const std::string& /*sourceHint*/,
                            const FbxImportOptions& options)
{
    // ── Assimp 第 1 パス: スキン/アニメーション用 ─────────────────────────
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* scene = importer.ReadFile(fbxPath, kBaseFlags);
    if (!scene || !scene->mRootNode) return false;

    const bool hasSkin = HasSkinning(scene);

    // ── Assimp 第 2 パス: 静的メッシュ用 (PreTransformVertices) ──────────
    Assimp::Importer staticImporter;
    const aiScene* meshScene = scene;
    if (!hasSkin) {
        staticImporter.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        meshScene = staticImporter.ReadFile(fbxPath, kStaticFlags);
        if (!meshScene || !meshScene->mRootNode) return false;
    }

    const fs::path fbxFsPath    = util::FileSystem::PathFromUtf8(fbxPath);
    const fs::path outDirPath   = util::FileSystem::PathFromUtf8(outputDir);
    const fs::path manifestDir  = outDirPath.parent_path();

    // ── 出力ディレクトリを作成 ────────────────────────────────────────────
    if (!util::FileSystem::EnsureDirectory(outDirPath)) return false;

    // ロールバックガード (失敗時に outputDir を削除)
    bool success = false;
    auto cleanup = [&] {
        if (!success) util::FileSystem::RemoveAll(outDirPath);
    };
    struct Guard { std::function<void()> fn; ~Guard() { fn(); } } guard{ cleanup };

    // ── コンテキスト構築 ─────────────────────────────────────────────────
    FbxImportContext ctx;
    ctx.scene                   = scene;
    ctx.meshScene               = meshScene;
    ctx.fbxPath                 = fbxPath;
    ctx.fbxDir                  = util::FileSystem::PathToUtf8(fbxFsPath.parent_path());
    ctx.baseName                = util::FileSystem::PathToUtf8(fbxFsPath.stem());
    ctx.outputDir               = outputDir;
    ctx.manifestDir             = util::FileSystem::PathToUtf8(manifestDir);
    ctx.unitScale               = ReadUnitScale(meshScene);
    ctx.hasSkin                 = hasSkin;
    ctx.normalMapConvention     = options.normalMapConvention;
    ctx.generateTexDescriptors  = options.generateTexDescriptors;
    ctx.defaultCompression      = options.defaultCompression;
    ctx.selectedMeshNames       = options.selectedMeshNames;
    ctx.selectedAnimNames       = options.selectedAnimNames;

    // ── パイプライン実行 ─────────────────────────────────────────────────
    auto pipeline = BuildPipeline();
    for (auto& exporter : pipeline) {
        if (!exporter->Export(ctx)) return false;
    }

    success = true;
    return true;
}

FbxScanResult FbxImportTool::Scan(const std::string& fbxPath)
{
    FbxScanResult result;
    Assimp::Importer importer;
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
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
