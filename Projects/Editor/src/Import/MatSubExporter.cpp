// FBZZ Engine
// MatSubExporter.cpp | fbzz::editor
// FBX aiMaterial → .mat TOML + textures/ コピー
// MaterialExporter を委譲して 1 マテリアルインデックスにつき 1 ファイルを生成する。
#include <Editor/Import/MatSubExporter.hpp>
#include <Editor/Import/MaterialExporter.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <assimp/material.h>
#include <assimp/scene.h>
#include <cctype>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace fbzz::editor {

namespace {

std::string SanitizeMaterialFileName(std::string name, uint32_t fallbackIndex)
{
    if (name.empty())
        name = "Material_" + std::to_string(fallbackIndex);

    for (char& c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-' && c != '.')
            c = '_';
    }
    if (name == "." || name == "..")
        name = "Material_" + std::to_string(fallbackIndex);
    return name;
}

std::string GetMaterialName(const aiMaterial* material, uint32_t fallbackIndex)
{
    aiString aiName;
    if (material && material->Get(AI_MATKEY_NAME, aiName) == AI_SUCCESS && aiName.length > 0)
        return aiName.C_Str();
    return "Material_" + std::to_string(fallbackIndex);
}

} // namespace

bool MatSubExporter::Export(FbxImportContext& ctx)
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

    std::unordered_map<std::string, uint32_t> usedNames;
    for (uint32_t matIdx = 0; matIdx < ms->mNumMaterials; ++matIdx) {
        // WHY: Unity と同じく FBX 内の material を index 順で全て .mat 化する。
        //      配置時は .fzasset の material slot index から同じ命名規則で .mat を引く。
        const bool skinned = ctx.hasSkin;
        std::string fileStem = SanitizeMaterialFileName(
            GetMaterialName(ms->mMaterials[matIdx], matIdx), matIdx);
        const uint32_t duplicateCount = usedNames[fileStem]++;
        if (duplicateCount > 0)
            fileStem += "_" + std::to_string(duplicateCount);
        const std::string matPath = util::FileSystem::PathToUtf8(
            matDir / (fileStem + ".mat"));

        if (!MaterialExporter::Export(
                ms->mMaterials[matIdx],
                ms,
                ctx.fbxDir,
                util::FileSystem::PathToUtf8(texDir),
                matPath,
                skinned,
                ctx.normalMapConvention == NormalMapConvention::OpenGL))
            return false;
    }

    return true;
}

} // namespace fbzz::editor
