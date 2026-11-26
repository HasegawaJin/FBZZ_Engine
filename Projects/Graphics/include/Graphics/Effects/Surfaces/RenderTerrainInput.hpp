/// @file    RenderTerrainInput.hpp
/// @brief   地形パッチと解決済み層資源の入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/Resources/RenderConstants.hpp>
#include <memory>
namespace fbzz::renderer {
inline constexpr size_t TERRAIN_FAR_LOD_INDEX = 3;
inline constexpr std::array<int, 4> TERRAIN_LOD_STEPS = {1, 2, 4, 8};

struct TerrainOcclusionColumn {
    math::Vector4 rectangle; ///< 遮蔽列が覆うローカル XZ 範囲 [m]。
    float minimumHeight = 0; ///< 範囲全体で確実に地形内部となる上端のローカル高さ [m]。
};
/// @note 描画と同じ穴・境界接続を持つ不変の CPU 形状。地形の編集時だけ再生成する。
struct TerrainOcclusionGeometry {
    std::vector<float> heights; ///< パッチのローカル高さサンプル [m]。
    std::array<std::vector<uint16_t>, TERRAIN_LOD_STEPS.size()> indices; ///< 穴と境界を反映した LOD ごとの頂点添字列。
    std::vector<TerrainOcclusionColumn> columns; ///< 全 LOD と境界を揃えた遮蔽列。地形の形状更新時に生成する。
};
/// @note TerrainGeometry.hlsli と共有する 32 バイトの形式。高さ格子は外周に 1 サンプルの余白を持つ。
struct TerrainGpuPatch {
    uint32_t originX = 0; ///< 全地形の高さ格子でパッチが始まる X 添字。
    uint32_t originZ = 0; ///< 全地形の高さ格子でパッチが始まる Z 添字。
    uint32_t columns = 0; ///< パッチの X 方向高さサンプル数。
    uint32_t rows = 0; ///< パッチの Z 方向高さサンプル数。
    uint32_t terrainColumns = 0; ///< 全地形の X 方向高さサンプル数。
    uint32_t terrainRows = 0; ///< 全地形の Z 方向高さサンプル数。
    float cellSize = 1.0f; ///< ローカル高さサンプル間の水平間隔 [m]。
    uint32_t padding = 0; ///< シェーダーの構造体サイズに合わせる予約領域。
};
static_assert(sizeof(TerrainGpuPatch) == 32);

struct TerrainStripIndices {
    ResourceHandle<BufferTag> buffer; ///< 三角形ストリップのインデックスバッファ。
    uint32_t count = 0; ///< 再開マーカーを含む buffer のインデックス数。
};

struct RenderTerrainPatch {
    std::shared_ptr<const TerrainOcclusionGeometry> occlusion; ///< ビュー間で共有する不変の CPU 遮蔽形状。
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer; ///< パッチの通常描画に使う頂点バッファ。
    std::array<renderer::ResourceHandle<renderer::BufferTag>, TERRAIN_LOD_STEPS.size()> indexBufferLOD; ///< LOD ごとのインデックスバッファ。
    std::array<uint32_t, TERRAIN_LOD_STEPS.size()> indexCountLOD = {}; ///< 各 indexBufferLOD が描画するインデックス数。
    math::Vector3 aabbMin; ///< パッチを囲うローカル AABB の最小点 [m]。
    math::Vector3 aabbMax; ///< パッチを囲うローカル AABB の最大点 [m]。
    TerrainGpuPatch gpuPatch{}; ///< 共有高さ格子からパッチを再構築する情報。
    std::array<uint32_t, TERRAIN_LOD_STEPS.size()> gpuStartIndexLOD{}; ///< LOD ごとの共有 GPU インデックス列内の開始位置 [要素]。
    TerrainStripIndices lod0Strip; ///< 最も細かい LOD のストリップ描画用入力。
};

struct RenderTerrainInput {
    uint32_t layer = 0; ///< ビューのカリングマスクに照合するレイヤー番号。
    bool selected = false; ///< 選択マスクを描く対象かどうか。
    bool fiberSurface = false; ///< 地形がファイバー表面描画を使うかどうか。
    bool fiberDistanceLod = true; ///< ビューからの距離でファイバーの密度を落とすかどうか。
    float fiberLodFar = 40.0f; ///< ファイバーの遠距離 LOD を決めるビューからの距離 [m]。
    float chunkWorldSize = 1; ///< 地形チャンクの代表的なワールド幅 [m]。
    float farLodDistance = 0.0f; ///< 最遠 LOD に切り替える距離 [m]。正値のときだけ最遠 LOD を許可する。
    size_t LODCount() const { return farLodDistance > 0.0f ? TERRAIN_LOD_STEPS.size() : TERRAIN_FAR_LOD_INDEX; }
    TerrainObjectCB constants{}; ///< 地形の変換と層描画に使うシェーダー定数。
    ResourceHandle<TextureTag> splatIndices; ///< 地形上の各位置で混ぜる材質層の添字テクスチャ。
    ResourceHandle<TextureTag> splatWeights; ///< splatIndices と同じ位置の層の寄与率テクスチャ。
    std::vector<RenderTerrainPatch> patches; ///< 遮蔽判定と描画の単位になる地形パッチ。
    /// @note GPU 側の不変形状はカメラ、影、選択の各パスで共有する。
    ResourceHandle<StructuredBufferTag> gpuHeights; ///< GPU が読む共有高さ格子。
    ResourceHandle<StructuredBufferTag> gpuPatches; ///< GPU が読むパッチごとの高さ格子範囲。
    ResourceHandle<BufferTag> gpuIndices; ///< GPU 選別経路が読む全パッチの共有インデックスバッファ。
    uint32_t gpuIndexCount = 0; ///< gpuIndices の有効インデックス数。
    uint32_t gpuVertexCapacity = 0; ///< GPU 経路がアドレス指定できる頂点数の上限。
    bool shadowBakeStatic = false; ///< 高さ、穴、境界、変換の版を検証でき、静的影のベイクに含められるかどうか。
    uint64_t shadowBakeGeometryHash = 0; ///< 静的影を再ベイクする必要がある形状の変更を識別するハッシュ。
};
} // namespace fbzz::renderer
