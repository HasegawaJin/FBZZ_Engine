// FBZZ Engine
// MaterialComponent.hpp | fbzz::scene
// マテリアル設定を GameObject に持たせるコンポーネント
// Unity Standard シェーダー相当のパラメータと 5 枚のテクスチャスロットを保持する。
// RenderSystem の SyncMaterial() がここの値を Renderer::Material へ反映する。
// Inspector / SceneSerializer で編集しやすい値型として保持し、
// GPU リソースは Material 側の ResourceHandle に持たせる。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <memory>
#include <string>

namespace fbzz::renderer {
class Material;
} // namespace fbzz::renderer

namespace fbzz::scene {

struct MaterialComponent {
    std::shared_ptr<renderer::Material> material;
    bool enabled = true;

    // ── Shader ─────────────────────────────────────────────────────────────
    // マテリアルが参照する HLSL ファイル。描画パス側の既定 PSO と組み合わせて使用する。
    std::string shaderPath;

    // ── Textures ────────────────────────────────────────────────────────────
    std::string albedoTexPath;        // t0  ベースカラー
    std::string normalTexPath;        // t1  法線マップ (OpenGL 規約 Y-up)
    std::string metallicRoughTexPath; // t2  G=roughness / B=metallic (glTF 規約)
    std::string emissiveTexPath;      // t3  エミッシブテクスチャ
    std::string aoTexPath;            // t4  アンビエントオクルージョン

    // ── Surface params ──────────────────────────────────────────────────────
    float albedoColor[4]    = { 1.0f, 1.0f, 1.0f, 1.0f }; // RGBA ベースカラー
    float metallic          = 0.0f;
    float roughness         = 0.8f;
    float normalStrength    = 1.0f; // 法線マップ強度
    float occlusionStrength = 1.0f; // AO 強度

    // ── Emissive ────────────────────────────────────────────────────────────
    float emissiveColor[3]  = { 1.0f, 1.0f, 1.0f }; // エミッシブ色
    float emissiveScale     = 0.0f;                   // エミッシブ強度

    // ── UV ──────────────────────────────────────────────────────────────────
    float uvTiling[2] = { 1.0f, 1.0f };
    float uvOffset[2] = { 0.0f, 0.0f };

    // ── Alpha ───────────────────────────────────────────────────────────────
    float alphaCutoff = 0.5f; // カットアウトシェーダー用しきい値

    const char* GetTypeName() const { return "Material"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",              enabled);
        r.Field("shaderPath",           shaderPath);
        r.Field("albedoTexPath",        albedoTexPath);
        r.Field("normalTexPath",        normalTexPath);
        r.Field("metallicRoughTexPath", metallicRoughTexPath);
        r.Field("emissiveTexPath",      emissiveTexPath);
        r.Field("aoTexPath",            aoTexPath);
        r.Field("albedoR",              albedoColor[0]);
        r.Field("albedoG",              albedoColor[1]);
        r.Field("albedoB",              albedoColor[2]);
        r.Field("albedoA",              albedoColor[3]);
        r.Field("metallic",             metallic);
        r.Field("roughness",            roughness);
        r.Field("normalStrength",       normalStrength);
        r.Field("occlusionStrength",    occlusionStrength);
        r.Field("emissiveR",            emissiveColor[0]);
        r.Field("emissiveG",            emissiveColor[1]);
        r.Field("emissiveB",            emissiveColor[2]);
        r.Field("emissiveScale",        emissiveScale);
        r.Field("uvTilingX",            uvTiling[0]);
        r.Field("uvTilingY",            uvTiling[1]);
        r.Field("uvOffsetX",            uvOffset[0]);
        r.Field("uvOffsetY",            uvOffset[1]);
        r.Field("alphaCutoff",          alphaCutoff);
    }
};

} // namespace fbzz::scene
