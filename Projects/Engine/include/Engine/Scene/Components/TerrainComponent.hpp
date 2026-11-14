/// @file    TerrainComponent.hpp
/// @brief   ハイトマップベースの地形コンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-31

/// @note TerrainComponent が高さ・スプラット・穴を一元管理し、TerrainRenderPass がチャンク分割と
/// @note GPU 転送を行う。層ごとのテクスチャ・タイリング・質感は layerMaterials が指す .mat が持つ。
/// @see Docs/design/terrain-layers.md
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/Reflection.hpp>
#include <Engine/Scene/TerrainSplat.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::scene {

/// @brief ハイトマップ頂点の両端を含む矩形。
struct TerrainHeightRegion {
    int minX = 0, minZ = 0, maxX = -1, maxZ = -1;

    [[nodiscard]] bool Empty() const { return minX > maxX || minZ > maxZ; }
    void Include(const TerrainHeightRegion& region)
    {
        if (region.Empty()) return;
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
    std::vector<float> heightData;

    int   columns   = 65;    ///< @note X 方向の頂点数 (2^n + 1 推奨)。
    int   rows      = 65;    ///< @note Z 方向の頂点数。
    float cellSize  = 2.0f;  ///< @note ローカル 1 マスの幅 [m]。
    float maxHeight = 20.0f; ///< @note heightData=1 のときのローカル高さ [m]。
    /// @}

    /// @name スプラット (頂点ごとに上位 4 層の番号と重み)
    /// @{
    /// @brief index = (z * columns + x) * 4 + slot。値は layerMaterials の添字。
    /// @note 書き込みは terrain_splat の関数を通して正準形 (合計 255・重み降順) を保つこと。
    std::vector<std::uint8_t> splatIndices;
    /// @brief splatIndices と同じ並びの重み [0, 255]。
    std::vector<std::uint8_t> splatWeights;
    /// @}

    /// @brief セルの穴。index = cz * (columns - 1) + cx、1 = 穴。空なら穴なし。
    /// @note 穴は三角形を作らないことで表す。描画・物理・NavMesh・Fiber が同じ配列を見る。
    std::vector<std::uint8_t> holeData;

    /// @note Additional terrain-local triangles share the terrain transform, collider and persistence.
    /// @note Winding faces the empty side: cave floors up, ceilings down. Holes remove the entrance heightfield.
    std::vector<math::Vector3> surfacePositions;
    std::vector<uint32_t> surfaceIndices;
    /// @note Empty uses terrain layers; a .mat overrides the added surface with one triplanar layer.
    std::string surfaceMaterial;

    /// @brief Assets/Terrain/*.terrain への参照。空文字ならシーン / Prefab 内に地形データを直接保存する。
    std::string terrainAssetPath;

    /// @brief 層ごとの .mat。添字がスプラットの番号になる。
    /// @note fzmat keys: diffuse/normal/ao_roughness/height/tilingX/tilingZ/normalStrength/roughness/
    /// @note ambientOcclusion/heightBlend/triplanar/triplanarSharpness/macroScale/macroStrength/
    /// @note autoBlendEnabled/autoBlendStrength/autoMinHeight/autoMaxHeight/autoHeightFade/
    /// @note autoMinSlope/autoMaxSlope/autoSlopeFade
    std::vector<std::string> layerMaterials = std::vector<std::string>(4);

    /// @brief 高さブレンドで «高い層が境界を押し広げる» 幅。重み [0, 1] と同じ単位。
    /// @see https://www.gamedeveloper.com/programming/advanced-terrain-texture-splatting
    float heightBlendDepth = 0.2f;

    /// @brief chunkSize × chunkSize マスのブロックごとにカリング・LOD を掛ける。
    int chunkSize = 32;

    /// @note 正値で描画専用の 8 セル間隔 LOD を有効にする。距離はワールド m、0 で従来の 3 段階。
    float farLodDistance = 0.0f;

    bool enabled            = true;
    bool heightDirty        = false; ///< @note true → TerrainRenderPass がメッシュを再構築する (穴の変更も含む)。
    bool splatDirty         = false; ///< @note true → スプラットと層テクスチャを再アップロードする。
    bool materialParamDirty = false; ///< @note true → 層パラメーターだけ再構築する。
    bool colliderDirty      = true;  ///< @note true → PhysicsSystem がコライダーを再構築する。
    /// @note 高さ・穴の派生データを失効させる実行時世代。保存せず、編集口か dirty 消費で進める。
    uint64_t surfaceRevision = 1;
    /// @note 複数の描画キャッシュが同じ更新を読めるように、抽出後も直近の変更範囲と適用可能な世代を保持する。
    struct HeightPreview {
        TerrainHeightRegion region;
        uint64_t baseRevision = 0, revision = 0;
        bool pending = false;
        bool heightChanged = true;
        bool topologyChanged = false;
    } heightPreview;
    /// @note Layer/hole authoring invalidates grass independently of water depth and collider revisions.
    uint64_t vegetationRevision = 1;
    /// @note The last published rectangle stays available to other renderer caches at the same base revision.
    struct SplatPreview {
        TerrainHeightRegion region;
        uint64_t baseRevision = 0, revision = 0;
        bool pending = false;
    } splatPreview;

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
        /// @note layerMaterials / heightData / splat / holeData は IReflector の対応型に収まらず、
        /// @note TerrainAssetSerializer と SceneSerializer が専用コードで読み書きする。
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
    /// @param localX テレインローカル座標 X (Transform 適用前)。
    /// @param localZ テレインローカル座標 Z (Transform 適用前)。
    /// @note 範囲外はクランプして継続する (Physics・足 IK から呼ばれるため assert しない)。穴でも高さを返す。
    /// @note Legacy heightfield authoring sample; ignores holes and added surfaces. Use TerrainSurfaceQuery for occupancy.
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
    /// @pre Inclusive cell rectangle within the current grid. Collision is invalidated by the stroke commit.
    void RequestHolePreview(const TerrainHeightRegion& cells)
    {
        if (!cells.Empty()) RequestSurfacePreview({cells.minX, cells.minZ, cells.maxX + 1, cells.maxZ + 1}, false, true);
    }
    void RequestSplatRebuild() { splatDirty = true; splatPreview = {}; }
    /// @note Inclusive vertex XZ rectangle. Whole-grid writers use RequestSplatRebuild, including when merging with a preview.
    void RequestSplatPreview(const TerrainHeightRegion& region)
    {
        if (splatDirty || region.Empty()) return;
        if (splatPreview.pending) splatPreview.region.Include(region);
        else splatPreview = {region, vegetationRevision, 0, true};
    }
    [[nodiscard]] bool HasPendingSplatUpdate() const { return splatDirty || splatPreview.pending; }
    void RequestMaterialRebuild() { materialParamDirty = true; }
    /// @note 穴は三角形の有無なので、描画メッシュとコライダーを作り直す。
    void RequestHoleRebuild() { RequestHeightRebuild(); }

    /// @return Invalid geometry leaves the current surface and revisions unchanged. Empty arrays remove it.
    FBZZ_ENGINE_API bool SetSurfaceGeometry(std::vector<math::Vector3> positions, std::vector<uint32_t> indices,
        std::string material = {});
    /// @note Direct surface array edits must call this; renderer, collider and water bounds share the revision.
    void RequestSurfaceRebuild() { RequestHeightRebuild(); }

    FBZZ_ENGINE_API bool SetHeightAtGrid(int x, int z, float worldHeight);
    /// @brief 頂点 (x, z) の layer の重みを weight [0, 1] に置く。他の層は比率を保って残りを分け合う。
    FBZZ_ENGINE_API bool PaintLayerAtGrid(int x, int z, int layer, float weight);
    /// @}

private:
    void RequestSurfacePreview(const TerrainHeightRegion& region, bool heights, bool topology)
    {
        if (region.Empty()) return;
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

} /// @note namespace fbzz::scene
