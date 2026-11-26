/// @file    TerrainComponent.hpp
/// @brief   ハイトマップベースの地形コンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-31

/// @note TerrainComponent が高さ・スプラット・穴を一元管理し、TerrainRenderPass がチャンク分割と
/// @note GPU 転送を行う。層ごとのテクスチャ・タイリング・質感は layerMaterials が指す .mat が持つ。
/// @see Docs/design/terrain-layers.md
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Reflection/Reflection.hpp>
#include <Engine/Scene/Terrain/TerrainSplat.hpp>
#include <Math/Scalar/MathUtils.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::scene {

/// @brief ハイトマップ頂点の両端を含む矩形。
struct TerrainHeightRegion {
    int minX = 0; ///< 矩形の最小 X 頂点添字。この頂点を範囲に含める。
    int minZ = 0; ///< 矩形の最小 Z 頂点添字。この頂点を範囲に含める。
    int maxX = -1; ///< 矩形の最大 X 頂点添字。この頂点を範囲に含める。
    int maxZ = -1; ///< 矩形の最大 Z 頂点添字。この頂点を範囲に含める。

    [[nodiscard]] bool Empty() const { return minX > maxX || minZ > maxZ; }
    void Include(const TerrainHeightRegion& region)
    {
        if (region.Empty()) {
            return;
        }
        if (Empty()) {
            *this = region;
            return;
        }
        minX = std::min(minX, region.minX);
        minZ = std::min(minZ, region.minZ);
        maxX = std::max(maxX, region.maxX);
        maxZ = std::max(maxZ, region.maxZ);
    }
};

/// @brief 地形の全データ。
struct TerrainComponent {
    /// @name ハイトマップ (CPU)
    /// @{
    /// @brief row-major: index = z * columns + x。値域 [-1, 1] → ワールド高さ = value * maxHeight。
    /// @note 0 を «フラットな基準面» とし、Lower ブラシで基準面より下へ掘れるようにする。
    std::vector<float> heightData; ///< Z 行・X 列の高さサンプル。maxHeight を掛けてローカル高さ（m）にする。

    int   columns   = 65; ///< X 方向の頂点数 (2^n + 1 推奨)。
    int   rows      = 65; ///< Z 方向の頂点数。
    float cellSize  = 2.0f; ///< ローカル 1 マスの幅 [m]。
    float maxHeight = 20.0f; ///< heightData=1 のときのローカル高さ [m]。
    /// @}

    /// @name スプラット (頂点ごとに上位 4 層の番号と重み)
    /// @{
    /// @brief index = (z * columns + x) * 4 + slot。値は layerMaterials の添字。
    /// @note 書き込みは terrain_splat の関数を通して正準形 (合計 255・重み降順) を保つこと。
    std::vector<std::uint8_t> splatIndices; ///< 各頂点の上位 4 層が参照する layerMaterials の添字を、重みの降順に格納する。
    std::vector<std::uint8_t> splatWeights; ///< splatIndices と同じ並びの重み [0, 255]。
    /// @}

    /// @brief セルの穴。index = cz * (columns - 1) + cx、1 = 穴。空なら穴なし。
    /// @note 穴は三角形を作らないことで表す。描画・物理・NavMesh・Fiber が同じ配列を見る。
    std::vector<std::uint8_t> holeData; ///< Z 行・X 列の地形セルの穴フラグ。1 は穴。空なら穴なし。

    /// @note 地形ローカルの追加三角形は、地形の Transform、コライダーと永続化を共有する。
    /// @note 三角形の巻き順は空間側を向ける。洞窟の床は上、天井は下。穴で入口の高さ場を除く。
    std::vector<math::Vector3> surfacePositions; ///< 洞窟などの追加面を構成する地形ローカルの頂点座標。
    std::vector<uint32_t> surfaceIndices; ///< 追加三角形を 3 要素ずつ指定する surfacePositions の添字列。
    std::string surfaceMaterial; ///< 空の場合は地形レイヤーを使い、.mat を指定すると追加面を単一のトライプラナー層で上書きする。

    std::string terrainAssetPath; ///< Assets/Terrain/*.terrain への参照。空文字ならシーン / Prefab 内に地形データを直接保存する。

    /// @brief 層ごとの .mat。添字がスプラットの番号になる。
    /// @note fzmat のキーは diffuse/normal/ao_roughness/height/tilingX/tilingZ/normalStrength/roughness/
    /// @note ambientOcclusion/heightBlend/triplanar/triplanarSharpness/macroScale/macroStrength/
    /// @note autoBlendEnabled/autoBlendStrength/autoMinHeight/autoMaxHeight/autoHeightFade/
    /// @note autoMinSlope/autoMaxSlope/autoSlopeFade
    std::vector<std::string> layerMaterials = std::vector<std::string>(4); ///< splatIndices の層番号に対応する .mat の参照列。

    /// @brief 高さブレンドで «高い層が境界を押し広げる» 幅。重み [0, 1] と同じ単位。
    /// @see https://www.gamedeveloper.com/programming/advanced-terrain-texture-splatting
    float heightBlendDepth = 0.2f; ///< 高さブレンドの境界幅。スプラット重みと同じ 0〜1 の尺度。

    int chunkSize = 32; ///< chunkSize × chunkSize マスのブロックごとにカリング・LOD を掛ける。

    float farLodDistance = 0.0f; ///< 正値で描画専用の 8 セル間隔 LOD を有効にする。距離はワールド m、0 で従来の 3 段階。

    bool enabled            = true; ///< この地形を有効なコンポーネントとして扱う。
    bool heightDirty        = false; ///< true → TerrainRenderPass がメッシュを再構築する (穴の変更も含む)。
    bool splatDirty         = false; ///< true → スプラットと層テクスチャを再アップロードする。
    bool materialParamDirty = false; ///< true → 層パラメーターだけ再構築する。
    bool colliderDirty      = true; ///< true → PhysicsSystem がコライダーを再構築する。
    uint64_t surfaceRevision = 1; ///< 高さ・穴の派生データを失効させる実行時世代。保存せず、編集口か dirty 消費で進める。
    /// @note 複数の描画キャッシュが同じ更新を読めるように、抽出後も直近の変更範囲と適用可能な世代を保持する。
    struct HeightPreview {
        TerrainHeightRegion region; ///< 高さまたは穴が変わった XZ 頂点範囲。
        uint64_t baseRevision = 0; ///< 差分を適用できる更新前の surfaceRevision。
        uint64_t revision = 0; ///< 最後に公開した高さ差分に対応する surfaceRevision。
        bool pending = false; ///< 描画抽出に向けて高さ・穴の変更範囲を集約している場合に true。
        bool heightChanged = true; ///< 頂点高さを更新する必要がある場合に true。
        bool topologyChanged = false; ///< 穴の編集などで三角形の接続を更新する必要がある場合に true。
    } heightPreview; ///< 最後に公開した高さ差分と、次の抽出に向けた更新範囲。
    uint64_t vegetationRevision = 1; ///< レイヤー・穴の編集は、水深やコライダーの版とは独立して草のキャッシュを無効化する。
    /// @note 最後に公開した矩形を保持し、同じ基準版の他の描画キャッシュからも利用できるようにする。
    struct SplatPreview {
        TerrainHeightRegion region; ///< スプラットの層番号または重みが変わった XZ 頂点範囲。
        uint64_t baseRevision = 0; ///< スプラット差分を適用できる更新前の vegetationRevision。
        uint64_t revision = 0; ///< 最後に公開したスプラット差分に対応する vegetationRevision。
        bool pending = false; ///< 描画抽出に向けてスプラットの範囲を集約している場合に true。
    } splatPreview; ///< 層の塗り替えを矩形単位で再転送するために保持する差分。

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
        r.Field("farLodDistance", farLodDistance);
        r.Field("terrainAssetPath", terrainAssetPath);
        r.FloatRange("heightBlendDepth", heightBlendDepth, 0.01f, 1.0f);
        // layerMaterials / heightData / splat / holeData は IReflector の対応型に収まらず、
        // TerrainAssetSerializer と SceneSerializer が専用コードで読み書きする。
    }
    /// @}

    /// @name 寸法
    /// @{
    [[nodiscard]] size_t VertexCount() const
    {
        return columns > 0 && rows > 0 ? static_cast<size_t>(columns) * static_cast<size_t>(rows) : 0u;
    }
    [[nodiscard]] size_t CellCount() const
    {
        return columns > 1 && rows > 1 ? static_cast<size_t>(columns - 1) * static_cast<size_t>(rows - 1) : 0u;
    }
    [[nodiscard]] int LayerCount() const { return static_cast<int>(layerMaterials.size()); }
    /// @}

    /// @name 初期化
    /// @{
    /// @brief スプラットを «全頂点が層 0 = 100%» に戻す。
    FBZZ_ENGINE_API void InitDefaultSplat();
    /// @return splatIndices / splatWeights が頂点数と一致するか。
    [[nodiscard]] FBZZ_ENGINE_API bool HasValidSplat() const;
    /// @brief サイズが合わないスプラットだけ既定値で作り直す。
    FBZZ_ENGINE_API void EnsureSplat();
    /// @note 呼び出し後に heightDirty = true を立てること。
    FBZZ_ENGINE_API void InitFlat(float height = 0.0f);
    /// @brief グリッドサイズを変更し、高さ・スプラット・穴を 2D コピー (切り捨て/パディング) で引き継ぐ。
    /// @note columns/rows を直接書き換えると配列サイズが合わず TerrainRenderPass の assert が落ちる。
    /// @note 呼び出し後に heightDirty / splatDirty / colliderDirty を立てること。
    FBZZ_ENGINE_API void Resize(int newColumns, int newRows);
    /// @}

    /// @brief 描画メッシュ・MeshCollider と同じ 2 三角形分割で高さを補間する。
    /// @param[in] localX テレインローカル座標 X (Transform 適用前)。
    /// @param[in] localZ テレインローカル座標 Z (Transform 適用前)。
    /// @note 範囲外はクランプして継続する (Physics・足 IK から呼ばれるため assert しない)。穴でも高さを返す。
    /// @note 従来の高さ場の編集用サンプル。穴と追加面を含む占有判定には TerrainSurfaceQuery を使う。
    FBZZ_ENGINE_API float GetHeightAt(float localX, float localZ) const;

    /// @brief 有限差分で計算した法線をバイリニア補間して返す。
    FBZZ_ENGINE_API math::Vector3 GetNormalAt(float localX, float localZ) const;

    /// @brief 整数グリッド座標 (x, z) に対して中心差分で法線を計算する。
    FBZZ_ENGINE_API math::Vector3 ComputeNormal(int x, int z) const;

    /// @name 層
    /// @{
    /// @return 頂点 (x, z) における layer の重み [0, 1]。範囲外やスプラット未初期化は 0。
    [[nodiscard]] FBZZ_ENGINE_API float GetLayerWeightAtGrid(int x, int z, int layer) const;
    /// @brief 層の .mat を差し替える。layer == LayerCount() なら末尾に追加する。
    /// @return 範囲外、または TERRAIN_MAX_LAYERS を超える追加なら false。
    FBZZ_ENGINE_API bool SetLayerMaterial(int layer, std::string materialPath);
    /// @return 追加した層の番号。上限なら -1。
    FBZZ_ENGINE_API int AddLayer(std::string materialPath);
    /// @brief 層を削除し、スプラットの番号を詰める。その層の重みは他の層で分け直す。
    FBZZ_ENGINE_API bool RemoveLayer(int layer);
    /// @brief 層の並びを変える。スプラットの番号も付け替えるので見た目は変わらない。
    FBZZ_ENGINE_API bool MoveLayer(int from, int to);
    /// @}

    /// @name 穴
    /// @{
    [[nodiscard]] FBZZ_ENGINE_API bool IsHoleCell(int cx, int cz) const;
    /// @return セルが範囲外なら false。変化が無くても範囲内なら true。
    FBZZ_ENGINE_API bool SetHoleCell(int cx, int cz, bool hole);
    /// @brief ローカル座標が穴のセルに入っているか。地形の外は false。
    [[nodiscard]] FBZZ_ENGINE_API bool IsHoleAtLocal(float localX, float localZ) const;
    [[nodiscard]] FBZZ_ENGINE_API size_t CountHoles() const;
    /// @}

    /// @name Script / Tool 向けの編集口
    /// @note データ更新と再構築要求を同時に行い、dirty フラグの立て忘れを防ぐ。
    /// @{
    void RequestHeightRebuild()
    {
        ++surfaceRevision;
        heightDirty = true;
        colliderDirty = true;
    }
    /// @pre region は現在の高さ場の範囲内。寸法・穴・追加メッシュが変わるときは全体を失効させる。
    /// @note ドラッグ中は描画を更新し、コライダーはストローク確定側で失効させる。
    void RequestHeightPreview(const TerrainHeightRegion& region) { RequestSurfacePreview(region, true, false); }
    /// @pre 現在の格子範囲内のセル矩形で、両端を含むこと。ストロークの確定時に衝突データを無効化する。
    void RequestHolePreview(const TerrainHeightRegion& cells)
    {
        if (!cells.Empty()) {
            RequestSurfacePreview({cells.minX, cells.minZ, cells.maxX + 1, cells.maxZ + 1}, false, true);
        }
    }
    void RequestSplatRebuild() { splatDirty = true; splatPreview = {}; }
    /// @note 両端を含む頂点 XZ 矩形。プレビューとの統合を含め、格子全体を更新する場合は RequestSplatRebuild を使う。
    void RequestSplatPreview(const TerrainHeightRegion& region)
    {
        if (splatDirty || region.Empty()) {
            return;
        }
        if (splatPreview.pending) {
            splatPreview.region.Include(region);
        }
        else {
            splatPreview = {region, vegetationRevision, 0, true};
        }
    }
    [[nodiscard]] bool HasPendingSplatUpdate() const { return splatDirty || splatPreview.pending; }
    void RequestMaterialRebuild() { materialParamDirty = true; }
    /// @note 穴は三角形の有無なので、描画メッシュとコライダーを作り直す。
    void RequestHoleRebuild() { RequestHeightRebuild(); }

    /// @return 不正な形状では現在の表面と各版を維持する。空の配列は追加面を削除する。
    FBZZ_ENGINE_API bool SetSurfaceGeometry(std::vector<math::Vector3> positions, std::vector<uint32_t> indices,
        std::string material = {});
    /// @note 追加面の配列を直接編集した後に呼ぶ。描画、コライダーと水面境界が同じ版を参照する。
    void RequestSurfaceRebuild() { RequestHeightRebuild(); }

    FBZZ_ENGINE_API bool SetHeightAtGrid(int x, int z, float worldHeight);
    /// @brief 頂点 (x, z) の layer の重みを weight [0, 1] に置く。他の層は比率を保って残りを分け合う。
    FBZZ_ENGINE_API bool PaintLayerAtGrid(int x, int z, int layer, float weight);
    /// @}

private:
    void RequestSurfacePreview(const TerrainHeightRegion& region, bool heights, bool topology)
    {
        if (region.Empty()) {
            return;
        }
        if (heightPreview.pending && heightPreview.revision == surfaceRevision) {
            heightPreview.region.Include(region);
            heightPreview.heightChanged |= heights;
            heightPreview.topologyChanged |= topology;
        } else {
            heightPreview = {region, surfaceRevision, 0, true, heights, topology};
        }
        heightPreview.revision = ++surfaceRevision;
    }

    /// @brief 1 点の高さをインデックスから取得する (クランプ境界)。
    float SampleHeight(int x, int z) const
    {
        x = (std::clamp)(x, 0, columns - 1);
        z = (std::clamp)(z, 0, rows    - 1);
        return heightData[static_cast<size_t>(z) * static_cast<size_t>(columns)
                        + static_cast<size_t>(x)] * maxHeight;
    }
};

}
