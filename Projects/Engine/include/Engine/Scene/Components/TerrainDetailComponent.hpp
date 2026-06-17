// FBZZ Engine
// TerrainDetailComponent.hpp | fbzz::scene
// Terrain 上に草・岩・花・低木などの小オブジェクトを GPU Instancing で大量描画する。
// Unity の Terrain Detail System / Unreal の Landscape Grass Output に相当する。
//
// WHY: TerrainComponent はハイトマップとスプラットを所有し、
//      描画戦略に関する責務を持たせたくない。Detail 配置データは独立した Component に
//      分離することで TerrainComponent を肥大化させずに済む。
//
// 設計書: Docs/System/Detail/overview.md
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::scene {

enum class DetailLayerType : uint8_t {
    Mesh      = 0, // 岩・花・低木など 3D メッシュ
    Billboard = 1, // カメラ向きクワッド（遠景フォールバック）
    Grass     = 2, // プロシージャル草ブレード（Phase 4）
};

// DetailLayer — 1 種類のオブジェクト配置ルールと視覚パラメータ
// 永続化対象。SceneSerializer が直接シリアライズする。
struct DetailLayer {
    DetailLayerType type = DetailLayerType::Mesh;

    // アセット参照
    std::string meshPath;        // Mesh レイヤー: .fzasset モデルパス
    std::string densityMapPath;  // グレースケール画像: 白=最大密度・黒=なし (空 = 配置なし)
    std::string texturePath;     // Billboard / Grass: アルベドテクスチャ

    // 配置
    float density        = 1.0f;  // 密度倍率 (0〜10); 1.0 = 4 個/m²
    float minScale       = 0.8f;
    float maxScale       = 1.2f;
    float alignToNormal  = 0.0f;  // 0=垂直固定, 1=法線に沿う (Phase 5)
    bool  randomYRotation = true;

    // 描画距離
    float drawDistance  = 50.0f;
    float fadeStartDist = 40.0f;  // フェードアウト開始 (Phase 5)

    // Grass 専用 (Phase 4)
    float bladeHeight   = 0.4f;
    float bladeWidth    = 0.05f;
    int   bladeSegments = 3;      // ブレード分割数
    float windStrength  = 1.0f;
    float windFrequency = 1.0f;
};

// DetailInstance — Mesh / Billboard インスタンスの GPU 転送データ (20 bytes)
// HLSL 側の DetailInstance 構造体と完全に一致させること。
struct DetailInstance {
    float posX, posY, posZ; // ワールド座標
    float rotY;             // Y 回転（ラジアン）
    float scale;            // 均等スケール
};
static_assert(sizeof(DetailInstance) == 20, "DetailInstance size mismatch");

// GrassInstance — 草インスタンスの GPU 転送データ (24 bytes, Phase 4)
struct GrassInstance {
    float posX, posY, posZ;
    float rotY;
    float scale;
    float windPhase; // ブレードごとの位相オフセット
};
static_assert(sizeof(GrassInstance) == 24, "GrassInstance size mismatch");

// DetailDensityMap — DetailTool がペイントした密度データ (ランタイム、非永続化)。
// 1 テクセル ≈ 2m² のグレースケールグリッド。0=インスタンスなし, 1=最大密度。
// 空のときは密度 0 として扱い、ユーザーが Paint するまで配置しない。
struct DetailDensityMap {
    std::vector<float> data;   // [z * width + x], 0..1
    int width  = 0;
    int height = 0;

    bool IsValid() const { return !data.empty() && width > 0 && height > 0; }

    float Sample(float u, float v) const {
        if (!IsValid()) return 0.0f;
        int tx = static_cast<int>(u * width);
        int tz = static_cast<int>(v * height);
        tx = tx < 0 ? 0 : (tx >= width  ? width  - 1 : tx);
        tz = tz < 0 ? 0 : (tz >= height ? height - 1 : tz);
        return data[static_cast<size_t>(tz) * static_cast<size_t>(width) + static_cast<size_t>(tx)];
    }
};

// DetailChunk — ランタイムのインスタンス配列とGPUバッファ。非永続化。
// 16m × 16m のチャンクに分割して管理する。
struct DetailChunk {
    int chunkX = 0;
    int chunkZ = 0;

    // [layerIndex] → Mesh/Billboard インスタンス配列
    std::vector<std::vector<DetailInstance>> instancesPerLayer;
    std::vector<renderer::ResourceHandle<renderer::StructuredBufferTag>> instanceBuffers;

    // [layerIndex] → Grass インスタンス配列 (Grass レイヤーのみ使用。他レイヤーは空)
    std::vector<std::vector<GrassInstance>>  grassInstancesPerLayer;
    std::vector<renderer::ResourceHandle<renderer::StructuredBufferTag>> grassInstanceBuffers;

    bool isDirty = true; // true → 次フレームで Bake が必要
};

struct TerrainDetailComponent {
    std::vector<DetailLayer> layers;

    // ランタイムキャッシュ。非永続化。
    // DetailRenderPass が所有・管理する（Bake 後に確定、Scene ロード時に再生成）。
    std::vector<DetailChunk> chunks;

    // DetailTool がペイントした密度マップ。layers と同じインデックスで対応。
    // 空エントリは「密度 0 / 配置なし」を意味する。
    std::vector<DetailDensityMap> densityMaps;

    // 最後に Bake した Terrain の World Transform。ランタイムキャッシュのため非永続化。
    // WHY: Terrain 描画・MeshCollider と同じ座標へ追従させ、親移動後も古い配置を残さない。
    math::Vector3     bakedWorldPosition = {};
    math::Quaternion  bakedWorldRotation = math::Quaternion::Identity();
    math::Vector3     bakedWorldScale    = math::Vector3::ONE;
    bool              hasBakedTransform  = false;

    bool enabled   = true;
    bool needsBake = true; // layers 変更時や Scene ロード時に true

    const char* GetTypeName() const { return "TerrainDetail"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        // layers は std::vector<struct> のため SceneSerializer が直接扱う
    }
};

} // namespace fbzz::scene
