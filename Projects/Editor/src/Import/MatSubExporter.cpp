/// @file    MatSubExporter.cpp
/// @brief   FBX aiMaterial → .mat TOML + textures/ コピー。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// MaterialExporter を委譲して 1 マテリアルインデックスにつき 1 ファイルを生成する。
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

} /// @note namespace

bool MatSubExporter::Export(FbxImportContext& ctx)
{
    const aiScene* ms = ctx.meshScene;
    if (!ms) return true;
    /// @note マテリアルが 1 つも無い FBX では何も書き出さない (先にフォルダを掘ると空のまま残るため)。
    if (ms->mNumMaterials == 0) return true;

    namespace fs = std::filesystem;
    /// @note .mat / textures は Library 側へ出す。GUID は原本 FBX の GUID とコンテナ内の相対パス
    ///       (`"materials/<name>.mat"` 等) から決定論的に導出するため .meta は不要。
    /// @note 埋め込みテクスチャは FBX から、外部参照は fbxDir または "<FBX名>.fbm/" から再コピーできるため
    ///       復元可能。プロジェクト外の絶対パス参照で実体がどちらにも無い場合だけ復元できない。
    /// @note materials/ と textures/ はここで事前に掘らない。書き込み側 (WriteText / DumpEmbeddedAsPng /
    ///       CopyFile) が書き込み直前に親ディレクトリを作るため、先に掘るとテクスチャ 0 枚の FBX で
    ///       空の textures/ が残ってしまう。
    const fs::path matDir = util::FileSystem::PathFromUtf8(ctx.manifestDir) / "materials";
    const fs::path texDir = util::FileSystem::PathFromUtf8(ctx.manifestDir) / "textures";

    std::unordered_map<std::string, uint32_t> usedNames;
    for (uint32_t matIdx = 0; matIdx < ms->mNumMaterials; ++matIdx) {
        /// @note FBX 内の material を index 順で全て .mat 化する。配置時は .fzasset の
        ///       material slot index から同じ命名規則で .mat を引く。
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
                ctx.baseName,
                util::FileSystem::PathToUtf8(texDir),
                matPath,
                skinned))
            return false;
    }

    return true;
}

} /// @note namespace fbzz::editor
