// FBZZ Engine
// TerrainComponent.hpp | fbzz::scene
// ハイトマップベースの地形コンポーネント
//
// WHY: 屋外シーンに広い起伏地形を置くには、個別の MeshRenderer では頂点データ管理と
//      高さクエリ（キャラクター・Physics）が分散してしまう。
//      TerrainComponent に heightData / splatData を一元管理させ、
//      TerrainRenderSystem がチャンク分割と GPU 転送を行う構造にすることで
//      「データ所有」と「描画戦略」を分離する。
//
// TerrainLayer : テクスチャ 1 レイヤーの定義（拡散光テクスチャ・法線マップ・タイリング）
// TerrainComponent : 地形の全データ（ハイトマップ・スプラットマップ・レイヤー・描画パラメータ）
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::scene {

// =============================================================================
// TerrainLayer — テクスチャ 1 レイヤーの定義
// TerrainComponent.layers に最大 4 つまで格納できる。
// スプラットマップの各チャンネル (R/G/B/A) がそれぞれ 1 レイヤーに対応する。
// =============================================================================
struct TerrainLayer {
    // AssetManager がロードするファイルパス。空文字 = 未設定（白テクスチャで代替）
    std::string diffusePath; // 例: "assets/terrain/grass_d.png"
    std::string normalPath;  // 例: "assets/terrain/grass_n.png"  空文字 = フラット法線

    // UV タイリング係数。地形全体 UV に掛け算する倍率。
    // WHY: 地形は広大なので UV を [0,1] のままにするとテクスチャが間延びする。
    //      tilingX / tilingZ で独立に設定して非正方形地形に対応する。
    float tilingX        = 8.0f;
    float tilingZ        = 8.0f;
    float normalStrength = 1.0f; // 法線マップの強度スケール [0, ∞)

    void Reflect(IReflector& r)
    {
        r.Field("diffusePath",    diffusePath);
        r.Field("normalPath",     normalPath);
        r.Field("tilingX",        tilingX);
        r.Field("tilingZ",        tilingZ);
        r.Field("normalStrength", normalStrength);
    }
};

// =============================================================================
// TerrainComponent — 地形の全データ
// =============================================================================
struct TerrainComponent {
    // ── ハイトマップ (CPU) ─────────────────────────────────────────────────────
    // row-major: index = z * columns + x
    // 値域 [0, 1] → ワールド高さ = value * maxHeight
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

    // ── テクスチャレイヤー (最大 4) ────────────────────────────────────────────
    std::vector<TerrainLayer> layers; // .size() <= 4

    // ── チャンク設定 ────────────────────────────────────────────────────────────
    // 地形を chunkSize × chunkSize マスのブロックに分割して描画。
    // チャンクあたりの頂点数 = (chunkSize + 1)^2
    // WHY: 大規模地形で全頂点を 1 DrawCall にまとめると GPU 転送量が爆発する。
    //      チャンク単位でフラスタムカリングを掛けることで不可視領域を排除する。
    int chunkSize = 32;

    bool enabled      = true;
    bool heightDirty  = false; // true → TerrainRenderSystem がメッシュを再構築する
    bool splatDirty   = false; // true → TerrainRenderSystem がスプラットマップを再アップロードする
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
        // layers / heightData / splatData は IReflector の対応型（float/int/bool/string）に
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
    // TerrainRenderSystem の BuildChunk からも呼ばれるため public にする。
    // WHY: friend 宣言で TerrainRenderSystem に密結合させるより、
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
