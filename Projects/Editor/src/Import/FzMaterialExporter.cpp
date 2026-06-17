// FBZZ Engine
// FzMaterialExporter.cpp | fbzz::editor
// aiMaterial → .mat (TOML) + テクスチャをそのまま texturesDir にコピー
#include <Editor/Import/FzMaterialExporter.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <DirectXTex.h>
#include <wincodec.h>
#include <assimp/material.h>
#include <assimp/scene.h>
#include <toml++/toml.hpp>
#include <filesystem>
#include <cstdint>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace fbzz::editor {

namespace {

struct TexSlot {
    aiTextureType type;
    const char*   key;
};
constexpr TexSlot kTexSlots[] = {
    { aiTextureType_DIFFUSE,           "albedo"    },
    { aiTextureType_NORMALS,           "normal"    },
    { aiTextureType_METALNESS,         "metallic"  },
    { aiTextureType_DIFFUSE_ROUGHNESS, "roughness" },
    { aiTextureType_AMBIENT_OCCLUSION, "ao"        },
    { aiTextureType_EMISSIVE,          "emissive"  },
};

// 圧縮済み埋め込みテクスチャ (PNG/JPEG バイト列) をそのままファイルに書き出す。
// 成功時はファイル名 (basename) を返す。
std::string DumpEmbedded(const aiScene* scene, int idx, const std::string& texturesDir)
{
    const aiTexture* tex = scene->mTextures[static_cast<uint32_t>(idx)];

    std::string filename = util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(tex->mFilename.C_Str()).filename());
    if (filename.empty()) {
        std::string fmt(tex->achFormatHint, strnlen(tex->achFormatHint, 4));
        filename = "embedded_" + std::to_string(idx) + (fmt.empty() ? ".png" : ("." + fmt));
    }

    const std::filesystem::path outPath = util::FileSystem::PathFromUtf8(texturesDir) / filename;
    return util::FileSystem::WriteBinary(outPath, tex->pcData, static_cast<size_t>(tex->mWidth))
        ? filename
        : std::string{};
}

// 非圧縮 BGRA8888 埋め込みテクスチャを DirectXTex WIC で PNG として保存する。
std::string DumpEmbeddedRaw(const aiScene* scene, int idx, const std::string& texturesDir)
{
    const aiTexture* tex = scene->mTextures[static_cast<uint32_t>(idx)];
    const std::string filename = "embedded_" + std::to_string(idx) + ".png";
    const std::filesystem::path outPath = util::FileSystem::PathFromUtf8(texturesDir) / filename;

    DirectX::ScratchImage img;
    if (FAILED(img.Initialize2D(DXGI_FORMAT_B8G8R8A8_UNORM,
                                 tex->mWidth, tex->mHeight, 1, 1)))
        return {};
    std::memcpy(img.GetPixels(), tex->pcData,
                static_cast<size_t>(tex->mWidth) * tex->mHeight * 4);

    if (FAILED(DirectX::SaveToWICFile(*img.GetImages(), DirectX::WIC_FLAGS_NONE,
                                       GUID_ContainerFormatPng, outPath.c_str())))
        return {};
    return filename;
}

// テクスチャを解決して texturesDir にコピーし、ファイル名 (basename) を返す。
std::string ResolveTexture(const aiScene* scene,
                            const std::string& rawPath,
                            const std::string& fbxDir,
                            const std::string& texturesDir)
{
    util::FileSystem::EnsureDirectory(util::FileSystem::PathFromUtf8(texturesDir));

    if (!rawPath.empty() && rawPath[0] == '*') {
        const int idx = std::stoi(rawPath.substr(1));
        if (idx < 0 || static_cast<uint32_t>(idx) >= scene->mNumTextures) return {};
        const aiTexture* tex = scene->mTextures[static_cast<uint32_t>(idx)];
        return (tex->mHeight == 0)
            ? DumpEmbedded(scene, idx, texturesDir)
            : DumpEmbeddedRaw(scene, idx, texturesDir);
    }

    // 外部ファイル → texturesDir にコピー
    fs::path srcPath = util::FileSystem::PathFromUtf8(rawPath);
    if (srcPath.is_relative()) srcPath = util::FileSystem::PathFromUtf8(fbxDir) / srcPath;
    if (!util::FileSystem::Exists(srcPath)) {
        const fs::path fallback = util::FileSystem::PathFromUtf8(fbxDir) / srcPath.filename();
        if (util::FileSystem::Exists(fallback)) srcPath = fallback;
        else {
            FBZZ_LOG_WARN("FzMaterialExporter: texture not found [%s]", rawPath.c_str());
            return {};
        }
    }

    const std::string filename = util::FileSystem::PathToUtf8(srcPath.filename());
    const fs::path dest = util::FileSystem::PathFromUtf8(texturesDir) / srcPath.filename();
    if (util::FileSystem::Exists(dest))
        return filename;
    return util::FileSystem::CopyFile(srcPath, dest) ? filename : std::string{};
}

// OpenGL 形式の法線マップ (Y 下向き) を DirectX 形式 (Y 上向き) に変換する。
// WHY: Blender/Maya のデフォルト書き出しが OpenGL 座標系のため、
//      DirectX エンジンで使用すると法線の Y 成分が反転して凸凹が逆になる。
bool FlipNormalMapGreen(const fs::path& texPath)
{
    DirectX::ScratchImage image;
    if (FAILED(DirectX::LoadFromWICFile(texPath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image))) {
        FBZZ_LOG_WARN("FzMaterialExporter: FlipGreen load failed [%s]",
                       texPath.string().c_str());
        return false;
    }
    DirectX::ScratchImage rgba;
    if (FAILED(DirectX::Convert(*image.GetImages(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                 DirectX::TEX_FILTER_DEFAULT, 0.0f, rgba)))
        return false;

    uint8_t* pixels = rgba.GetPixels();
    const size_t pixelCount = rgba.GetPixelsSize() / 4;
    for (size_t i = 0; i < pixelCount; ++i)
        pixels[i * 4 + 1] = static_cast<uint8_t>(255u - pixels[i * 4 + 1]);

    return SUCCEEDED(DirectX::SaveToWICFile(*rgba.GetImages(), DirectX::WIC_FLAGS_NONE,
                                             GUID_ContainerFormatPng, texPath.c_str()));
}

} // namespace

bool FzMaterialExporter::Export(const aiMaterial* material,
                                  const aiScene* scene,
                                  const std::string& fbxDir,
                                  const std::string& texturesDir,
                                  const std::string& outputPath,
                                  bool skinned,
                                  bool flipGreenChannel)
{
    toml::table tbl;
    tbl.insert("version", int64_t{ 1 });
    tbl.insert("shader", skinned
        ? std::string{ "Assets/Shaders/Material/Skinned/SkinnedPBR.hlsl" }
        : std::string{ "Assets/Shaders/Material/Surface/PBR.hlsl" });

    aiString matName;
    if (material->Get(AI_MATKEY_NAME, matName) == AI_SUCCESS)
        tbl.insert("name", std::string(matName.C_Str()));

    toml::table paramsTbl;
    aiColor4D baseColor;
    if (material->Get(AI_MATKEY_BASE_COLOR,    baseColor) == AI_SUCCESS ||
        material->Get(AI_MATKEY_COLOR_DIFFUSE, baseColor) == AI_SUCCESS)
        paramsTbl.insert("albedo",
                         toml::array{ baseColor.r, baseColor.g, baseColor.b, baseColor.a });

    float metallic = 0.0f, roughness = 0.5f;
    material->Get(AI_MATKEY_METALLIC_FACTOR,  metallic);
    material->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness);
    paramsTbl.insert("metallic",  metallic);
    paramsTbl.insert("roughness", roughness);
    tbl.insert("params", std::move(paramsTbl));

    toml::table texTbl;
    for (const auto& slot : kTexSlots) {
        aiString texPath;
        if (material->GetTexture(slot.type, 0, &texPath) == AI_SUCCESS) {
            const std::string filename =
                ResolveTexture(scene, texPath.C_Str(), fbxDir, texturesDir);
            if (!filename.empty()) {
                if (flipGreenChannel && std::string_view(slot.key) == "normal") {
                    const fs::path fullPath =
                        util::FileSystem::PathFromUtf8(texturesDir) / filename;
                    if (!FlipNormalMapGreen(fullPath))
                        FBZZ_LOG_WARN("FzMaterialExporter: FlipGreen failed [%s]", filename.c_str());
                }
                texTbl.insert(slot.key, filename);
            }
        }
    }
    if (!texTbl.empty())
        tbl.insert("textures", std::move(texTbl));

    std::ostringstream out;
    out << tbl << '\n';
    if (!util::FileSystem::WriteText(util::FileSystem::PathFromUtf8(outputPath), out.str())) {
        FBZZ_LOG_ERROR("FzMaterialExporter: cannot open [%s]", outputPath.c_str());
        return false;
    }
    return true;
}

} // namespace fbzz::editor
