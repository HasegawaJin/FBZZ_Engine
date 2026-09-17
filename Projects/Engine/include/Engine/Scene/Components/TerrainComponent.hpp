/// @file    TerrainComponent.hpp
/// @brief   ハイトマップベースの地形コンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// TerrainComponent が heightData / splatData を一元管理し、TerrainRenderPass がチャンク分割と
/// GPU 転送を行うことで「データ所有」と「描画戦略」を分離する。各レイヤーのテクスチャ・
/// タイリング・roughness 等は layerMaterials[4] が指す .mat で管理し、地形間で再利用できる。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::scene {

/// @brief 地形の全データ。
struct TerrainComponent {
    /// @name ハイトマップ (CPU)
    /// @{
    /// @brief row-major: index = z * columns + x。値域 [-1, 1] → ワールド高さ = value * maxHeight。
    /// @note 0 を「フラットな基準面」とし、Lower ブラシで基準面より下へ掘れるようにする。
    ///       maxHeight は正負両方向の最大振幅として扱う。
    std::vector<float> heightData;

    int   columns   = 65;    ///< X 方向の頂点数 (2^n + 1 推奨: チャンク境界整合・LOD 二分割容易)
    int   rows      = 65;    ///< Z 方向の頂点数
    float cellSize  = 2.0f;  ///< 1 マスのワールド単位幅 [m]
    float maxHeight = 20.0f; ///< heightData=1 のときのワールド高さ [m]
    /// @}

    /// @name スプラットマップ (CPU, RGBA8 unorm)
    /// @{
    /// @brief index = (z * columns + x) * 4 + channel (0=R 1=G 2=B 3=A)。R=layer0, G=layer1,
    ///        B=layer2, A=layer3、各チャンネルは [0, 255]。
    /// @note 4 チャンネルの合計が 255 になるよう正規化する。シェーダーが除算するため float 変換は
    ///       描画時に行い、CPU では uint8 のまま保持する。
    std::vector<uint8_t> splatData;
    /// @}

    /// @brief Assets/Terrain/*.terrain への参照。空文字ならシーン / Prefab 内に地形データを直接保存する。
    /// @note 大きい地形では heightData / splatData がシーンファイルを肥大化させるため、
    ///       Prefab や Scene には参照だけを残し、重い編集データは専用アセットへ分離する。
    std::string terrainAssetPath;

    /// @brief レイヤーマテリアル参照 (4 レイヤー)。各要素が独立した .mat を指す。
    /// @note fzmat keys: diffuse/normal/ao_roughness/tilingX/tilingZ/normalStrength/roughness/
    ///       ambientOcclusion/autoBlendEnabled/autoBlendStrength/autoMinHeight/autoMaxHeight/
    ///       autoHeightFade/autoMinSlope/autoMaxSlope/autoSlopeFade
    std::array<std::string, 4> layerMaterials;

    /// @brief 地形を chunkSize × chunkSize マスのブロックに分割して描画する。チャンクあたりの
    ///        頂点数 = (chunkSize + 1)^2。
    /// @note 大規模地形で全頂点を 1 DrawCall にまとめると GPU 転送量が爆発するため、チャンク単位で
    ///       フラスタムカリングを掛けて不可視領域を排除する。
    int chunkSize = 32;

    bool enabled      = true;
    bool heightDirty        = false; ///< true → TerrainRenderPass がメッシュを再構築する
    bool splatDirty         = false; ///< true → スプラットマップ + レイヤーテクスチャを再アップロードする
    bool materialParamDirty = false; ///< true → マテリアルパラメータ CB のみ再構築 (テクスチャ再アップロード不要)
    bool colliderDirty = true; ///< true → PhysicsSystem がコライダーを再構築する (初期値 true で初回自動構築)

    /// @name Reflection (Inspector / Serializer 対応)
    /// @{
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
        /// @note layerMaterials は string 配列のため SceneSerializer が専用コードで読み書きする。
        ///       heightData / splatData も IReflector の対応型に収まらず、SceneSerializer が
        ///       TerrainComponent を直接扱う専用コードで読み書きする。
    }
    /// @}

    /// @name ハイトマップ初期化ヘルパー
    /// @{
    /// @brief スプラットマップを layer0=100% の初期状態へ戻す。
    /// @note splatData が空のまま保存されると .terrain が不完全になり、PaintTool や
    ///       TerrainRenderPass の「4チャンネル正規化済み」という前提を満たせなくなる。
    void InitDefaultSplat()
    {
        const size_t vertexCount = static_cast<size_t>(columns) * static_cast<size_t>(rows);
        splatData.assign(vertexCount * 4u, 0u);
        for (size_t i = 0; i < vertexCount; ++i)
            splatData[i * 4u] = 255u;
    }

    /// @note 呼び出し後に heightDirty = true を立てること。
    void InitFlat(float height = 0.0f)
    {
        heightData.assign(static_cast<size_t>(columns) * static_cast<size_t>(rows), height / maxHeight);
        InitDefaultSplat();
    }
    /// @}

    /// @brief グリッドサイズを変更する。heightData / splatData を 2D コピー (切り捨て/パディング) で引き継ぐ。
    /// @note columns/rows を直接書き換えるだけでは heightData のサイズが合わず
    ///       TerrainRenderPass の assert が落ちるため、必ずこの関数で一括変更する。
    ///       呼び出し後に heightDirty / splatDirty / colliderDirty を立てること。
    void Resize(int newColumns, int newRows)
    {
        const int copyCols = (std::min)(columns, newColumns);
        const int copyRows = (std::min)(rows,    newRows);
        const size_t newVerts = static_cast<size_t>(newColumns) * static_cast<size_t>(newRows);

        std::vector<float>   newHeight(newVerts, 0.0f);
        std::vector<uint8_t> newSplat (newVerts * 4u, 0u);

        for (int z = 0; z < copyRows; ++z) {
            for (int x = 0; x < copyCols; ++x) {
                const size_t src = static_cast<size_t>(z) * static_cast<size_t>(columns)    + static_cast<size_t>(x);
                const size_t dst = static_cast<size_t>(z) * static_cast<size_t>(newColumns) + static_cast<size_t>(x);
                if (!heightData.empty()) newHeight[dst] = heightData[src];
                if (!splatData.empty()) {
                    newSplat[dst * 4u + 0] = splatData[src * 4u + 0];
                    newSplat[dst * 4u + 1] = splatData[src * 4u + 1];
                    newSplat[dst * 4u + 2] = splatData[src * 4u + 2];
                    newSplat[dst * 4u + 3] = splatData[src * 4u + 3];
                }
            }
        }

        /// @note コピー範囲外の新規領域を layer0=100% で初期化する。
        for (int z = 0; z < newRows; ++z) {
            for (int x = 0; x < newColumns; ++x) {
                if (z < copyRows && x < copyCols) continue;
                newSplat[(static_cast<size_t>(z) * static_cast<size_t>(newColumns)
                          + static_cast<size_t>(x)) * 4u] = 255u;
            }
        }

        columns    = newColumns;
        rows       = newRows;
        heightData = std::move(newHeight);
        splatData  = std::move(newSplat);
    }

    /// @brief 高さをバイリニア補間で取得する。
    /// @param localX テレインローカル座標 X (Transform 適用前)。
    /// @param localZ テレインローカル座標 Z (Transform 適用前)。
    /// @note 範囲外はクランプして継続する (assert せず、Physics・足 IK から呼ばれるため)。
    float GetHeightAt(float localX, float localZ) const;

    /// @brief 有限差分で計算した法線をバイリニア補間して返す。
    math::Vector3 GetNormalAt(float localX, float localZ) const;

    /// @brief 整数グリッド座標 (x, z) に対して有限差分で法線を計算する。
    /// @note TerrainRenderPass の BuildChunk からも呼ばれるため public。friend で密結合させるより
    ///       汎用的な計算関数として公開する方がテスト・拡張しやすい。
    math::Vector3 ComputeNormal(int x, int z) const;

    /// @brief Script / Tool から地形データを変更するための安全な入口。
    /// @note heightData / splatData を直接編集すると dirty フラグを立て忘れやすいため、
    ///       データ更新と再構築要求をここで同時に行う。
    void RequestHeightRebuild()
    {
        heightDirty = true;
        colliderDirty = true;
    }

    void RequestSplatRebuild()
    {
        splatDirty = true;
    }

    void RequestMaterialRebuild()
    {
        materialParamDirty = true;
    }

    bool SetHeightAtGrid(int x, int z, float worldHeight)
    {
        if (columns <= 0 || rows <= 0) return false;
        if (heightData.size() != static_cast<size_t>(columns) * static_cast<size_t>(rows))
            InitFlat();

        x = (std::clamp)(x, 0, columns - 1);
        z = (std::clamp)(z, 0, rows - 1);
        const float safeMaxHeight = (std::max)(maxHeight, 0.0001f);
        heightData[static_cast<size_t>(z) * static_cast<size_t>(columns) + static_cast<size_t>(x)] =
            (std::clamp)(worldHeight / safeMaxHeight, -1.0f, 1.0f);
        RequestHeightRebuild();
        return true;
    }

    bool PaintLayerAtGrid(int x, int z, int layer, float weight)
    {
        if (columns <= 0 || rows <= 0 || layer < 0 || layer >= 4) return false;
        if (splatData.size() != static_cast<size_t>(columns) * static_cast<size_t>(rows) * 4u)
            InitDefaultSplat();

        x = (std::clamp)(x, 0, columns - 1);
        z = (std::clamp)(z, 0, rows - 1);
        const size_t base =
            (static_cast<size_t>(z) * static_cast<size_t>(columns) + static_cast<size_t>(x)) * 4u;
        splatData[base + static_cast<size_t>(layer)] =
            static_cast<uint8_t>((std::clamp)(weight, 0.0f, 1.0f) * 255.0f);

        int sum = 0;
        for (int i = 0; i < 4; ++i)
            sum += splatData[base + static_cast<size_t>(i)];
        if (sum <= 0) {
            splatData[base + static_cast<size_t>(layer)] = 255u;
        } else {
            int normalizedSum = 0;
            for (int i = 0; i < 3; ++i) {
                uint8_t v = static_cast<uint8_t>(
                    static_cast<int>(splatData[base + static_cast<size_t>(i)]) * 255 / sum);
                splatData[base + static_cast<size_t>(i)] = v;
                normalizedSum += v;
            }
            splatData[base + 3u] = static_cast<uint8_t>((std::clamp)(255 - normalizedSum, 0, 255));
        }
        RequestSplatRebuild();
        return true;
    }

    bool SetLayerMaterial(int layer, std::string materialPath)
    {
        if (layer < 0 || layer >= 4) return false;
        layerMaterials[static_cast<size_t>(layer)] = std::move(materialPath);
        RequestSplatRebuild();
        RequestMaterialRebuild();
        return true;
    }

private:

    /// @brief 1 点の高さをインデックスから取得する (クランプ境界)。
    float SampleHeight(int x, int z) const
    {
        x = (std::clamp)(x, 0, columns - 1);
        z = (std::clamp)(z, 0, rows    - 1);
        return heightData[static_cast<size_t>(z) * static_cast<size_t>(columns)
                        + static_cast<size_t>(x)] * maxHeight;
    }
};

} // namespace fbzz::scene
