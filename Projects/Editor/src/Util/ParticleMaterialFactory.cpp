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
#include <cctype>

namespace fbzz::editor {
namespace {

// Particle 用 .mat の置き場。テンプレートが同梱している 25 枚もここにある。
constexpr const char* kParticleMaterialDir = "Assets/Materials/Particles";

[[nodiscard]] const char* BlendModeName(scene::ParticleBlendMode blend)
{
    switch (blend) {
    case scene::ParticleBlendMode::Alpha:         return "AlphaBlend";
    case scene::ParticleBlendMode::Premultiplied: return "Premultiplied";
    default:                                      return "Additive";
    }
}

} // namespace

bool IsTextureAssetPath(const std::string& path)
{
    // 判定もエンジン側の 1 つに寄せる。ここに写しを置くと «Editor は受けるのに
    // エンジンは救わない» 拡張子が生まれる。
    return asset::IsParticleTexturePath(path);
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

    // 命名規則はエンジン側 (ParticleMaterialSettings) が正本。ここで組み立て直すと、
    // «作る先» と «ParticlePass が探す先» が食い違って移行が空振りする。
    const std::string assetMaterial =
        asset::ParticleMaterialPathForTexture(assetTexture, blend);
    if (assetMaterial.empty()) return fail("素材名を作れませんでした");
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

    // 見出しは «素材名» だけを出す。パスは下の「素材:」行と重複する。
    const std::string materialName =
        assetMaterial.substr(assetMaterial.find_last_of('/') + 1);

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
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(materialPath);
    const auto* material = asset::AssetManager::Get<asset::MaterialAsset>(handle);
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
