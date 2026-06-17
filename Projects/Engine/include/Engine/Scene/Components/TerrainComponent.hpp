// FBZZ Engine
// TerrainComponent.hpp | fbzz::scene
// ハイトマップベースの地形コンポーネント
//
// WHY: 屋外シーンに広い起伏地形を置くには、個別の MeshRenderer では頂点データ管理と
//      高さクエリ（キャラクター・Physics）が分散してしまう。
//      TerrainComponent に heightData / splatData を一元管理させ、
//      TerrainRenderPass がチャンク分割と GPU 転送を行う構造にすることで
//      「データ所有」と「描画戦略」を分離する。
//
// 各レイヤーのテクスチャ・タイリング・roughness 等は layerMaterials[4] が指す .fzmat で管理する。
// WHY: レイヤーごとに独立した .fzmat にすることで複数地形間でマテリアルを再利用できる。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::scene {

// =============================================================================
// TerrainComponent — 地形の全データ
// =============================================================================
struct TerrainComponent {
    // ── ハイトマップ (CPU) ─────────────────────────────────────────────────────
    // row-major: index = z * columns + x
    // 値域 [-1, 1] → ワールド高さ = value * maxHeight
    // WHY: 0 を「フラットな基準面」とし、Lower ブラシで地面を基準面より下へ掘れるようにする。
    //      maxHeight は正負両方向の最大振幅として扱う。
    std::vector<float> heightData;

    int   columns   = 129;    // X 方向の頂点数（2^n + 1 推奨: チャンク境界整合・LOD 二分割容易）
    int   rows      = 129;    // Z 方向の頂点数
    float cellSize  = 1.0f;  // 1 マスのワールド単位幅 [m]
    float maxHeight = 30.0f; // heightData=1 のときのワールド高さ [m]

    // ── スプラットマップ (CPU, RGBA8 unorm) ────────────────────────────────────
    // index = (z * columns + x) * 4 + channel  (0=R 1=G 2=B 3=A)
    // R=layer0, G=layer1, B=layer2, A=layer3, 各チャンネルは [0, 255]
    // WHY: 4 チャンネルの合計が 255 になるよう正規化する。
    //      シェーダーが除算するため float 変換は描画時に行い、CPU では uint8 のまま保持する。
    std::vector<uint8_t> splatData;

    // ── 外部 Terrain Asset ─────────────────────────────────────────────────────
    // Assets/Terrain/*.fbzzterrain への参照。空文字ならシーン / Prefab 内に地形データを直接保存する。
    // WHY: 大きい地形では heightData / splatData がシーンファイルを肥大化させるため、
    //      Prefab や Scene には参照だけを残し、重い編集データは専用アセットへ分離する。
    std::string terrainAssetPath;

    // ── レイヤーマテリアル参照 (4 レイヤー) ───────────────────────────────────
    // 各要素が独立した .fzmat を指す。fzmat keys: "diffuse", "normal", "ao_roughness",
    // "tilingX", "tilingZ", "normalStrength", "roughness", "ambientOcclusion",
    // "autoBlendEnabled", "autoBlendStrength", "autoMinHeight", "autoMaxHeight",
    // "autoHeightFade", "autoMinSlope", "autoMaxSlope", "autoSlopeFade"
    std::array<std::string, 4> layerMaterials;

    // ── チャンク設定 ────────────────────────────────────────────────────────────
    // 地形を chunkSize × chunkSize マスのブロックに分割して描画。
    // チャンクあたりの頂点数 = (chunkSize + 1)^2
    // WHY: 大規模地形で全頂点を 1 DrawCall にまとめると GPU 転送量が爆発する。
    //      チャンク単位でフラスタムカリングを掛けることで不可視領域を排除する。
    int chunkSize = 32;

    bool enabled      = true;
    bool heightDirty        = false; // true → TerrainRenderPass がメッシュを再構築する
    bool splatDirty         = false; // true → スプラットマップテクスチャ + レイヤーテクスチャを再アップロードする
    bool materialParamDirty = false; // true → マテリアルパラメータ CB のみ再構築（テクスチャ再アップロード不要）
    bool colliderDirty = true; // true → PhysicsSystem がコライダーを再構築する（初期値 true で初回自動構築）

    // ── Reflection (Inspector / Serializer 対応) ─────────────────────────────
    const char* GetTypeName() const { return "Terrain"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",    enabled);
        r.Field("columns",    columns);
        r.Field("rows",       rows);
        r.Field("cellSize",   cellSize);
        r.Field("maxHeight",  maxHeight);
        r.Field("chunkSize",  chunkSize);
        r.Field("terrainAssetPath", terrainAssetPath);
        // layerMaterials は string 配列のため SceneSerializer が専用コードで読み書きする。
        // heightData / splatData は IReflector の対応型（float/int/bool/string）に
        // 収まらないため、SceneSerializer が TerrainComponent を直接扱う専用コードで読み書きする。
    }

    // ── ハイトマップ初期化ヘルパー ────────────────────────────────────────────
    // 呼び出し後に heightDirty = true を立てること。
    void InitFlat(float height = 0.0f)
    {
        heightData.assign(static_cast<size_t>(columns) * static_cast<size_t>(rows), height / maxHeight);
    }

    // ── 高さクエリ (バイリニア補間) ──────────────────────────────────────────
    // (localX, localZ) はテレインローカル座標（Transform 適用前）。
    // 範囲外はクランプして継続（assert せず、Physics・足 IK から呼ばれるため）。
    float GetHeightAt(float localX, float localZ) const;

    // ── 法線クエリ (バイリニア補間) ──────────────────────────────────────────
    // 有限差分で計算した法線をバイリニア補間して返す。
    math::Vector3 GetNormalAt(float localX, float localZ) const;

    // ── グリッド座標ベースの法線計算 ─────────────────────────────────────────
    // 整数グリッド座標 (x, z) に対して有限差分で法線を計算する。
    // TerrainRenderPass の BuildChunk からも呼ばれるため public にする。
    // WHY: friend 宣言で TerrainRenderPass に密結合させるより、
    //      汎用的な計算関数として公開する方がテスト・拡張しやすい。
    math::Vector3 ComputeNormal(int x, int z) const;

private:

    // 1 点の高さをインデックスから取得する（クランプ境界）。
    float SampleHeight(int x, int z) const
    {
        x = std::clamp(x, 0, columns - 1);
        z = std::clamp(z, 0, rows    - 1);
        return heightData[static_cast<size_t>(z) * static_cast<size_t>(columns)
                        + static_cast<size_t>(x)] * maxHeight;
    }
};

} // namespace fbzz::scene
