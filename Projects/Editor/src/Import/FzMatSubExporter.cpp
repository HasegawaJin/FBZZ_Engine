// FBZZ Engine
// FzMatSubExporter.cpp | fbzz::editor
// FBX aiMaterial → .mat TOML + textures/ コピー
// 既存 FzMaterialExporter を委譲して 1 マテリアルインデックスにつき 1 ファイルを生成する。
#include <Editor/Import/FzMatSubExporter.hpp>
#include <Editor/Import/FzMaterialExporter.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <assimp/material.h>
#include <assimp/scene.h>
#include <filesystem>
#include <vector>

namespace fbzz::editor {

bool FzMatSubExporter::Export(FbxImportContext& ctx)
{
    const aiScene* ms = ctx.meshScene;
    if (!ms) return true;

    namespace fs = std::filesystem;
    const fs::path outDir = util::FileSystem::PathFromUtf8(ctx.outputDir);
    const fs::path matDir = outDir / "materials";
    const fs::path texDir = outDir / "textures";

    if (!util::FileSystem::EnsureDirectory(matDir) ||
        !util::FileSystem::EnsureDirectory(texDir))
        return false;

    std::vector<std::string> matPaths(ms->mNumMaterials);
    for (uint32_t mi = 0; mi < ms->mNumMeshes; ++mi) {
        const uint32_t matIdx = ms->mMeshes[mi]->mMaterialIndex;
        if (matIdx >= ms->mNumMaterials) continue;
        if (!matPaths[matIdx].empty()) continue; // 重複スキップ

        const bool skinned = ms->mMeshes[mi]->HasBones();
        const std::string matPath = util::FileSystem::PathToUtf8(
            matDir / ("mat_" + std::to_string(matIdx) + ".mat"));

        if (!FzMaterialExporter::Export(
                ms->mMaterials[matIdx],
                ms,
                ctx.fbxDir,
                util::FileSystem::PathToUtf8(texDir),
                matPath,
                skinned,
                ctx.normalMapConvention == NormalMapConvention::OpenGL))
            return false;
        matPaths[matIdx] = matPath;
    }

    return true;
}

} // namespace fbzz::editor
