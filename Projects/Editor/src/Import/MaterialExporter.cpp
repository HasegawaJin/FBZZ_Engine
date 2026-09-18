/// @file    MaterialExporter.cpp
/// @brief   aiMaterial → .mat (TOML) + テクスチャをそのまま texturesDir にコピー。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Editor/Import/MaterialExporter.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <DirectXTex.h>
#include <wincodec.h>
#include <assimp/material.h>
#include <assimp/scene.h>
#include <stb_image.h>
#include <toml++/toml.hpp>
#include <filesystem>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
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

/// @brief 圧縮済み埋め込みテクスチャをデコードする。
/// @note DirectXTex.lib は WIC/TGA/HDR のメモリローダーを含まないため、DDS 以外は stb_image を使う。
bool DecodeEmbeddedTexture(const aiTexture* texture, DirectX::ScratchImage& decoded)
{
    std::string format(texture->achFormatHint, strnlen(texture->achFormatHint, 4));
    for (char& c : format)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    const auto* bytes = static_cast<const uint8_t*>(static_cast<const void*>(texture->pcData));
    const size_t byteCount = static_cast<size_t>(texture->mWidth);
    if (format == "dds") {
        return SUCCEEDED(DirectX::LoadFromDDSMemory(
            bytes, byteCount, DirectX::DDS_FLAGS_NONE, nullptr, decoded));
    }

    if (byteCount > static_cast<size_t>(std::numeric_limits<int>::max())) return false;

    int width = 0;
    int height = 0;
    int sourceChannels = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        bytes, static_cast<int>(byteCount), &width, &height, &sourceChannels, STBI_rgb_alpha);
    if (!pixels || width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        return false;
    }

    const HRESULT initResult = decoded.Initialize2D(
        DXGI_FORMAT_R8G8B8A8_UNORM,
        static_cast<size_t>(width), static_cast<size_t>(height), 1, 1);
    if (FAILED(initResult)) {
        stbi_image_free(pixels);
        return false;
    }

    std::memcpy(decoded.GetPixels(), pixels,
                static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    stbi_image_free(pixels);
    return true;
}

/// @brief FBX 埋め込みテクスチャを必ず PNG として保存する。
/// @note mHeight == 0 は圧縮バイト列、それ以外は aiTexel (BGRA8888) 配列として扱う。
std::string DumpEmbeddedAsPng(const aiTexture* texture,
                              int textureIndex,
                              const std::string& texturesDir)
{
    /// @note 元名を残すことで normal/roughness 等の型推定を維持し、index で同名衝突を避ける。
    std::string fileStem = util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(texture->mFilename.C_Str()).stem());
    if (fileStem.empty()) fileStem = "embedded";
    for (char& c : fileStem) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') c = '_';
    }
    const std::string filename =
        fileStem + "_" + std::to_string(textureIndex) + ".png";
    const std::filesystem::path outPath = util::FileSystem::PathFromUtf8(texturesDir) / filename;
    if (util::FileSystem::Exists(outPath)) return filename;

    DirectX::ScratchImage source;
    if (texture->mHeight == 0) {
        if (!DecodeEmbeddedTexture(texture, source)) return {};
    } else {
        if (FAILED(source.Initialize2D(DXGI_FORMAT_B8G8R8A8_UNORM,
                                       texture->mWidth, texture->mHeight, 1, 1)))
            return {};
        std::memcpy(source.GetPixels(), texture->pcData,
                    static_cast<size_t>(texture->mWidth) * texture->mHeight * sizeof(aiTexel));
    }

    const DirectX::Image* image = source.GetImage(0, 0, 0);
    if (!image) return {};

    DirectX::ScratchImage rgba;
    if (DirectX::IsCompressed(image->format)) {
        if (FAILED(DirectX::Decompress(*image, DXGI_FORMAT_R8G8B8A8_UNORM, rgba)))
            return {};
        image = rgba.GetImage(0, 0, 0);
    } else if (image->format != DXGI_FORMAT_R8G8B8A8_UNORM &&
               image->format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        if (FAILED(DirectX::Convert(*image, DXGI_FORMAT_R8G8B8A8_UNORM,
                                    DirectX::TEX_FILTER_DEFAULT, 0.0f, rgba)))
            return {};
        image = rgba.GetImage(0, 0, 0);
    }

    if (!image) return {};

    /// @note textures/ の作成はここまで遅延させる。先に掘ると、埋め込みテクスチャが無い/デコード失敗の
    ///       FBX でも空フォルダだけが残るため、書き出す直前に親を用意する。
    if (!util::FileSystem::EnsureParentDirectory(outPath)) return {};

    if (FAILED(DirectX::SaveToWICFile(*image, DirectX::WIC_FLAGS_NONE,
                                       GUID_ContainerFormatPng, outPath.c_str())))
        return {};
    return filename;
}

/// テクスチャを解決して texturesDir にコピーし、ファイル名 (basename) を返す。
std::string ResolveTexture(const aiScene* scene,
                            const std::string& rawPath,
                            const std::string& fbxDir,
                            const std::string& fbxBaseName,
                            const std::string& texturesDir)
{
    /// @note ここで texturesDir を掘らない。参照が解決できなければ空フォルダだけが残るため、書き出しを行う
    ///       DumpEmbeddedAsPng と FileSystem::CopyFile 側で書き込み直前に親ディレクトリを作る。

    /// @note Assimp は埋め込みを "*0" または元ファイル名で返すため、両形式を公式 API で解決する。
    const auto [embedded, embeddedIndex] = scene->GetEmbeddedTextureAndIndex(rawPath.c_str());
    if (embedded && embeddedIndex >= 0)
        return DumpEmbeddedAsPng(embedded, embeddedIndex, texturesDir);

    /// @note 外部ファイル → texturesDir にコピー
    fs::path srcPath = util::FileSystem::PathFromUtf8(rawPath);
    if (srcPath.is_relative()) srcPath = util::FileSystem::PathFromUtf8(fbxDir) / srcPath;
    if (!util::FileSystem::Exists(srcPath)) {
        /// @note DCC が絶対パスを書いた FBX では原本の場所を再現できない。実体は FBX の隣か、
        ///       FBX SDK が埋め込みメディアを展開した `"<FBX名>.fbm/"` のどちらかに残っている。
        const fs::path fbxFsDir = util::FileSystem::PathFromUtf8(fbxDir);
        const fs::path candidates[] = {
            fbxFsDir / srcPath.filename(),
            fbxFsDir / util::FileSystem::PathFromUtf8(fbxBaseName + ".fbm") / srcPath.filename(),
        };
        bool resolved = false;
        for (const fs::path& candidate : candidates) {
            if (!util::FileSystem::Exists(candidate)) continue;
            srcPath  = candidate;
            resolved = true;
            break;
        }
        if (!resolved) return {};
    }

    const std::string filename = util::FileSystem::PathToUtf8(srcPath.filename());
    const fs::path dest = util::FileSystem::PathFromUtf8(texturesDir) / srcPath.filename();
    if (util::FileSystem::Exists(dest))
        return filename;
    return util::FileSystem::CopyFile(srcPath, dest) ? filename : std::string{};
}

/// @brief OpenGL 形式の法線マップ (Y 下向き) を DirectX 形式 (Y 上向き) に変換する。
/// @note Blender/Maya の既定書き出しは OpenGL 座標系のため、DirectX で使うと Y 成分が反転し凸凹が逆になる。
bool FlipNormalMapGreen(const fs::path& texPath)
{
    DirectX::ScratchImage image;
    if (FAILED(DirectX::LoadFromWICFile(texPath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image)))
        return false;
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

bool MaterialExporter::Export(const aiMaterial* material,
                                  const aiScene* scene,
                                  const std::string& fbxDir,
                                  const std::string& fbxBaseName,
                                  const std::string& texturesDir,
                                  const std::string& outputPath,
                                  bool skinned,
                                  bool flipGreenChannel)
{
    /// @note マテリアルの既知スロットに現れない画像も含め、FBX 内包テクスチャを全て PNG 化する
    ///       (Assimp が UNKNOWN/HEIGHT 等へ分類した画像も欠落させないため)。
    /// @note ここでディレクトリを掘らない。mNumTextures == 0 だとループが回らず空の textures/ だけが
    ///       残るため、作成は DumpEmbeddedAsPng に任せる。
    for (uint32_t textureIndex = 0; textureIndex < scene->mNumTextures; ++textureIndex) {
        if (DumpEmbeddedAsPng(scene->mTextures[textureIndex],
                              static_cast<int>(textureIndex), texturesDir).empty())
            return false;
    }

    toml::table tbl;
    tbl.insert("version", int64_t{ 1 });
    tbl.insert("shader", skinned
        ? std::string{ "Assets/Shaders/Material/Skinned/SkinnedPBR.hlsl" }
        : std::string{ "Assets/Shaders/Material/Surface/PBR.hlsl" });
    /// @note 通常材質の描画経路はプロジェクト設定が決めるため、インポート時に固定しない。
    tbl.insert("render_path", std::string{ "auto" });
    tbl.insert("mesh_type", skinned ? std::string{ "skinned" } : std::string{ "surface" });

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
    /// @note 拡張 PBR ローブは既存 FBX の外観を変えないよう無効値で初期化し、
    ///       必要なマテリアルだけが .mat 上で有効化できるようにする。
    paramsTbl.insert("clearcoat",          0.0f);
    paramsTbl.insert("clearcoatRoughness", 0.10f);
    paramsTbl.insert("sheen",              0.0f);
    paramsTbl.insert("anisotropy",         0.0f);
    paramsTbl.insert("sheenColor",         toml::array{ 1.0f, 1.0f, 1.0f });
    tbl.insert("params", std::move(paramsTbl));

    toml::table texTbl;
    for (const auto& slot : kTexSlots) {
        aiString texPath;
        if (material->GetTexture(slot.type, 0, &texPath) == AI_SUCCESS) {
            const std::string filename =
                ResolveTexture(scene, texPath.C_Str(), fbxDir, fbxBaseName, texturesDir);
            if (!filename.empty()) {
                if (flipGreenChannel && std::string_view(slot.key) == "normal") {
                    const fs::path fullPath =
                        util::FileSystem::PathFromUtf8(texturesDir) / filename;
                    FlipNormalMapGreen(fullPath);
                }
                /// @note マテリアルは元画像を直接参照する。インポート設定は元画像隣の
                ///       "<画像>.meta" サイドカーが担うため、.mat 側は source を指すだけでよい。
                texTbl.insert(slot.key, filename);
            }
        }
    }
    if (!texTbl.empty())
        tbl.insert("textures", std::move(texTbl));

    std::ostringstream out;
    out << tbl << '\n';
    if (!util::FileSystem::WriteText(util::FileSystem::PathFromUtf8(outputPath), out.str()))
        return false;
    return true;
}

} // namespace fbzz::editor
