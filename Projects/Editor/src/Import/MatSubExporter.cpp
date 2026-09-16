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

} // namespace

bool MatSubExporter::Export(FbxImportContext& ctx)
{
    const aiScene* ms = ctx.meshScene;
    if (!ms) return true;
    // マテリアルが 1 つも無い FBX では何も書き出さない。
    // WHY: 以前はここを抜けて materials/ と textures/ を先に掘っていたため、
    //      書き出すものが無くても空フォルダだけが残っていた。
    if (ms->mNumMaterials == 0) return true;

    namespace fs = std::filesystem;
    // .mat / textures とも Library 側へ出す (隠蔽)。GUID は原本 FBX の GUID と
    // コンテナ内の相対パス ("materials/<name>.mat" / "textures/<name>.png") から
    // 決定論的に導出されるため、.meta も Library の永続性も要らない。
    //
    // NOTE (textures の再生成条件): 埋め込みテクスチャは FBX 自体から復元できる。
    //   外部参照は FBX の隣 (fbxDir) と "<FBX名>.fbm/" から再コピーされるため、
    //   原本が Assets 内にあれば復元できる。
    //   FBX がプロジェクト外の絶対パスを参照し、実体もそのどちらにも無い場合だけは復元できない。
    //
    // WHY (ディレクトリを事前に作らない): materials/ は .mat を書く FileSystem::WriteText が、
    //   textures/ は DumpEmbeddedAsPng と FileSystem::CopyFile が、それぞれ書き込み直前に
    //   親ディレクトリを作る。ここで先に掘ると、テクスチャを 1 枚も持たない FBX で
    //   空の textures/ が必ず残ってしまう (実際に Boss モデル等で発生していた)。
    const fs::path matDir = util::FileSystem::PathFromUtf8(ctx.manifestDir) / "materials";
    const fs::path texDir = util::FileSystem::PathFromUtf8(ctx.manifestDir) / "textures";

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
                ctx.baseName,
                util::FileSystem::PathToUtf8(texDir),
                matPath,
                skinned,
                ctx.normalMapConvention == NormalMapConvention::OpenGL))
            return false;
    }

    return true;
}

} // namespace fbzz::editor
