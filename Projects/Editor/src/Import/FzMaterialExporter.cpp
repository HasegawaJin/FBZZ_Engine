// FBZZ Engine
// FzMaterialExporter.cpp | fbzz::editor
// aiMaterial → .fzmat (TOML) + テクスチャをそのまま texturesDir にコピー
#include <Editor/Import/FzMaterialExporter.hpp>
#include <Engine/Core/Logger.hpp>
#include <DirectXTex.h>
#include <wincodec.h>
#include <assimp/material.h>
#include <assimp/scene.h>
#include <toml++/toml.hpp>
#include <filesystem>
#include <fstream>
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

    std::string filename = fs::path(tex->mFilename.C_Str()).filename().string();
    if (filename.empty()) {
        std::string fmt(tex->achFormatHint, strnlen(tex->achFormatHint, 4));
        filename = "embedded_" + std::to_string(idx) + (fmt.empty() ? ".png" : ("." + fmt));
    }

    const std::string outPath = (fs::path(texturesDir) / filename).string();
    std::ofstream out(outPath, std::ios::binary);
    if (!out) return {};
    out.write(reinterpret_cast<const char*>(tex->pcData),
              static_cast<std::streamsize>(tex->mWidth));
    return out.good() ? filename : std::string{};
}

// 非圧縮 BGRA8888 埋め込みテクスチャを DirectXTex WIC で PNG として保存する。
std::string DumpEmbeddedRaw(const aiScene* scene, int idx, const std::string& texturesDir)
{
    const aiTexture* tex = scene->mTextures[static_cast<uint32_t>(idx)];
    const std::string filename = "embedded_" + std::to_string(idx) + ".png";
    const std::string outPath  = (fs::path(texturesDir) / filename).string();

    DirectX::ScratchImage img;
    if (FAILED(img.Initialize2D(DXGI_FORMAT_B8G8R8A8_UNORM,
                                 tex->mWidth, tex->mHeight, 1, 1)))
        return {};
    std::memcpy(img.GetPixels(), tex->pcData,
                static_cast<size_t>(tex->mWidth) * tex->mHeight * 4);

    const std::wstring wpath(outPath.begin(), outPath.end());
    if (FAILED(DirectX::SaveToWICFile(*img.GetImages(), DirectX::WIC_FLAGS_NONE,
                                       GUID_ContainerFormatPng, wpath.c_str())))
        return {};
    return filename;
}

// テクスチャを解決して texturesDir にコピーし、ファイル名 (basename) を返す。
std::string ResolveTexture(const aiScene* scene,
                            const std::string& rawPath,
                            const std::string& fbxDir,
                            const std::string& texturesDir)
{
    std::error_code ec;
    fs::create_directories(texturesDir, ec);

    if (!rawPath.empty() && rawPath[0] == '*') {
        const int idx = std::stoi(rawPath.substr(1));
        if (idx < 0 || static_cast<uint32_t>(idx) >= scene->mNumTextures) return {};
        const aiTexture* tex = scene->mTextures[static_cast<uint32_t>(idx)];
        return (tex->mHeight == 0)
            ? DumpEmbedded(scene, idx, texturesDir)
            : DumpEmbeddedRaw(scene, idx, texturesDir);
    }

    // 外部ファイル → texturesDir にコピー
    fs::path srcPath(rawPath);
    if (srcPath.is_relative()) srcPath = fs::path(fbxDir) / srcPath;
    if (!fs::exists(srcPath)) {
        const fs::path fallback = fs::path(fbxDir) / srcPath.filename();
        if (fs::exists(fallback)) srcPath = fallback;
        else {
            FBZZ_LOG_WARN("FzMaterialExporter: texture not found [%s]", rawPath.c_str());
            return {};
        }
    }

    const std::string filename = srcPath.filename().string();
    const fs::path dest = fs::path(texturesDir) / srcPath.filename();
    if (!fs::exists(dest))
        fs::copy_file(srcPath, dest, ec);
    return ec ? std::string{} : filename;
}

} // namespace

bool FzMaterialExporter::Export(const aiMaterial* material,
                                  const aiScene* scene,
                                  const std::string& fbxDir,
                                  const std::string& texturesDir,
                                  const std::string& outputPath)
{
    toml::table tbl;

    aiString matName;
    if (material->Get(AI_MATKEY_NAME, matName) == AI_SUCCESS)
        tbl.insert("name", std::string(matName.C_Str()));

    aiColor4D baseColor;
    if (material->Get(AI_MATKEY_BASE_COLOR,    baseColor) == AI_SUCCESS ||
        material->Get(AI_MATKEY_COLOR_DIFFUSE, baseColor) == AI_SUCCESS)
        tbl.insert("base_color",
                   toml::array{ baseColor.r, baseColor.g, baseColor.b, baseColor.a });

    float metallic = 0.0f, roughness = 0.5f;
    material->Get(AI_MATKEY_METALLIC_FACTOR,  metallic);
    material->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness);
    tbl.insert("metallic_factor",  metallic);
    tbl.insert("roughness_factor", roughness);

    toml::table texTbl;
    for (const auto& slot : kTexSlots) {
        aiString texPath;
        if (material->GetTexture(slot.type, 0, &texPath) == AI_SUCCESS) {
            const std::string filename =
                ResolveTexture(scene, texPath.C_Str(), fbxDir, texturesDir);
            if (!filename.empty())
                texTbl.insert(slot.key, filename);
        }
    }
    if (!texTbl.empty())
        tbl.insert("textures", std::move(texTbl));

    std::ofstream out(outputPath);
    if (!out) {
        FBZZ_LOG_ERROR("FzMaterialExporter: cannot open [%s]", outputPath.c_str());
        return false;
    }
    out << tbl << '\n';
    return out.good();
}

} // namespace fbzz::editor
