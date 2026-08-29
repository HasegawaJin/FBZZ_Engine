/// @file    ParticleMaterialFactory.cpp
/// @brief   テクスチャ 1 枚から Particle 用 .mat を用意する共有ファクトリ
/// @author  Hasegawa Jin
/// @date    2026-08-22
#include <Editor/Util/ParticleMaterialFactory.hpp>

#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/Toast.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <algorithm>
#include <array>
#include <cctype>

namespace fbzz::editor {
namespace {

// Particle 用 .mat の置き場。テンプレートが同梱している 25 枚もここにある。
constexpr const char* kParticleMaterialDir = "Assets/Materials/Particles";

constexpr std::array<const char*, 5> kTextureExtensions = {
    ".png", ".tga", ".dds", ".jpg", ".jpeg"
};

[[nodiscard]] std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

[[nodiscard]] const char* BlendModeName(scene::ParticleBlendMode blend)
{
    switch (blend) {
    case scene::ParticleBlendMode::Alpha:         return "AlphaBlend";
    case scene::ParticleBlendMode::Premultiplied: return "Premultiplied";
    default:                                      return "Additive";
    }
}

// ファイル名に使う短い識別子。BlendModeName と分けているのは、
// .mat 本文には parser が受ける正式名 ("AlphaBlend") を、ファイル名には
// 既存アセットと揃う短い名 ("Alpha") を使うため。
[[nodiscard]] const char* BlendFileSuffix(scene::ParticleBlendMode blend)
{
    switch (blend) {
    case scene::ParticleBlendMode::Alpha:         return "Alpha";
    case scene::ParticleBlendMode::Premultiplied: return "Premultiplied";
    default:                                      return "Additive";
    }
}

// "flame_03" -> "Flame03"。区切りを落として各語の頭を大文字にするだけの決定的な変換。
// WHY: 同梱の 25 枚と同じ名前へ落ちることで、テンプレートが既に使っている .mat を
//      作り直さずそのまま再利用できる。命名がぶれると同じ素材の .mat が二重に増える。
[[nodiscard]] std::string ToMaterialStem(const std::string& textureStem)
{
    std::string result;
    result.reserve(textureStem.size());
    bool atWordStart = true;
    for (const char c : textureStem) {
        if (c == '_' || c == '-' || c == ' ' || c == '.') {
            atWordStart = true;
            continue;
        }
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc)) continue;
        result.push_back(atWordStart ? static_cast<char>(std::toupper(uc)) : c);
        atWordStart = false;
    }
    return result.empty() ? std::string("Particle") : result;
}

} // namespace

bool IsTextureAssetPath(const std::string& path)
{
    const std::string ext = ToLower(util::FileSystem::GetExtension(path));
    return std::find(kTextureExtensions.begin(), kTextureExtensions.end(), ext)
        != kTextureExtensions.end();
}

std::string EnsureParticleMaterial(const std::string& projectRoot,
                                   const std::string& texturePath,
                                   scene::ParticleBlendMode blend,
                                   std::string* outError)
{
    const auto fail = [outError](const char* message) -> std::string {
        if (outError != nullptr) *outError = message;
        return {};
    };

    if (texturePath.empty()) return fail("テクスチャが指定されていません");
    if (!IsTextureAssetPath(texturePath))
        return fail("テクスチャではありません (.png/.tga/.dds/.jpg)");

    const std::string assetTexture = NormalizeAssetPath(texturePath);
    const std::string textureDisk = ToProjectAssetDiskPath(projectRoot, assetTexture);
    if (!util::FileSystem::Exists(textureDisk))
        return fail("テクスチャが見つかりません");

    std::string stem = util::FileSystem::GetFilename(assetTexture);
    if (const size_t dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);

    const std::string materialName =
        ToMaterialStem(stem) + "_" + BlendFileSuffix(blend) + ".mat";
    const std::string assetMaterial = std::string(kParticleMaterialDir) + "/" + materialName;
    const std::string materialDisk = ToProjectAssetDiskPath(projectRoot, assetMaterial);

    // 既存ならそのまま使う。上書きすると担当者が手で調整した .mat を壊す。
    if (util::FileSystem::Exists(materialDisk)) return assetMaterial;

    if (!util::FileSystem::EnsureDirectory(
            ToProjectAssetDiskPath(projectRoot, kParticleMaterialDir)))
        return fail("Assets/Materials/Particles を作成できません");

    // テクスチャ参照は guid で書く。パスで書くと、素材を別フォルダーへ移した瞬間に
    // .mat 側だけが取り残されて「移動しただけで白くなる」ことになる。
    std::string textureRef = assetTexture;
    if (const std::string guid = asset::AssetDatabase::GuidFromPath(textureDisk); !guid.empty())
        textureRef = std::string(asset::AssetDatabase::kGuidPrefix) + guid;

    std::string body;
    body += "# FBZZ Engine\n";
    body += "# " + materialName + "\n";
    body += "# Particle 用に自動生成されたマテリアル。\n";
    body += "# WHY: 素材 1 枚につき 1 マテリアルを置くことで、複数の Emitter から\n";
    body += "#      同じ素材設定を共有できる。\n";
    body += "# shader を空にしてあるのは «組み込み Particle.hlsl で描く» の意味。\n";
    body += "# 手続きシェーダーを使うときだけ shader を書く (要 render_path = \"particle\")。\n";
    body += "# 素材: " + assetTexture + "\n";
    body += "version = 1\n";
    body += "shader = \"\"\n";
    body += "blend_mode = \"" + std::string(BlendModeName(blend)) + "\"\n";
    body += "double_sided = true\n";
    body += "depth_write = false\n";
    body += "render_queue = 3000\n";
    body += "render_path = \"particle\"\n";
    body += "mesh_type = \"surface\"\n";
    body += "\n[textures]\n";
    body += "albedo = \"" + textureRef + "\"\n";
    body += "\n[params]\n";
    body += "albedo = [1.0, 1.0, 1.0, 1.0]\n";

    if (!util::FileSystem::WriteText(materialDisk, body))
        return fail("マテリアルを書き出せません");

    // 生成直後に .meta を確定させる。索引に載る前に他所から guid 参照されると
    // 「作ったのに壊れた参照」になるため、ここで自己修復を 1 回走らせておく。
    (void)asset::AssetDatabase::GuidFromPath(materialDisk);
    // 実体が生まれる前に一度でも参照されていると、失敗結果がキャッシュへ焼き付いている。
    asset::AssetManager::FlushFailed();

    return assetMaterial;
}

const asset::ParticleMaterialSettings* ResolveParticleMaterialSettings(
    const std::string& materialPath)
{
    if (materialPath.empty()) return nullptr;
    const auto handle = asset::AssetManager::LoadMaterial(materialPath);
    const auto* material = asset::AssetManager::GetMaterial(handle);
    return material != nullptr ? &material->particle : nullptr;
}

bool ParticleMaterialField(const char* label, std::string& materialPath,
                           const std::string& projectRoot,
                           scene::ParticleBlendMode blendForNewMaterial)
{
    // 手入力・ピッカー・D&D のすべてでテクスチャを許す。ここを .mat だけに絞ると
    // 「Asset Browser から .png を掴んでも受け付けない」状態へ戻る。
    constexpr const char* kFilter = ".mat,.png,.tga,.dds,.jpg,.jpeg";
    if (!widgets::AssetPathField(label, materialPath, kFilter, projectRoot)) return false;

    if (!IsTextureAssetPath(materialPath)) return true;

    const std::string texture = materialPath;
    std::string error;
    const std::string material =
        EnsureParticleMaterial(projectRoot, texture, blendForNewMaterial, &error);
    if (material.empty()) {
        // 変換できないテクスチャを materialPath に残すと 1x1 白で描かれ、
        // 「貼ったのに何も出ない」に戻ってしまう。入れる前に捨てる。
        materialPath.clear();
        Toast::Error("Material を作れませんでした: " + error);
        return true;
    }
    materialPath = material;
    Toast::Success(util::FileSystem::GetFilename(texture) + " -> "
                   + util::FileSystem::GetFilename(material));
    return true;
}

} // namespace fbzz::editor
