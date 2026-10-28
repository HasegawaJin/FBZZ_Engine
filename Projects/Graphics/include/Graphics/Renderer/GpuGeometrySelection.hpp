/// @file    GpuGeometrySelection.hpp
/// @brief   ビュー別 GPU 可視判定・投影 LOD・間接描画引数を生成する共通サービス。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include <Graphics/Renderer/ResourceHandle.hpp>

namespace fbzz::renderer {

class IRenderer;
class ResourceManager;

inline constexpr uint32_t kGpuGeometryMaxLods = 4;
inline constexpr uint32_t kGpuGeometrySelectionGroupSize = 64;
inline constexpr uint32_t kGpuGeometryIndexedArgumentStride = 24;

/// @brief 同じ VB / IB を使う候補の 1 つの LOD index 範囲。
/// @note cellSizeWorld はこの LOD のワールド単位の最大格子幅で、投影ピクセルから選択する。
struct GpuGeometryLodRange {
    uint32_t indexCount = 0;
    uint32_t startIndex = 0;
    int32_t baseVertex = 0;
    uint32_t vertexCount = 0;
    float cellSizeWorld = 0.0f;
    std::array<float, 3> padding{};
};
static_assert(sizeof(GpuGeometryLodRange) == 32);

/// @brief 同じ VB / IB batch 内の 1 つの可視・LOD 判定候補。
/// @note bounds は変位・アニメーションなどを含む保守的な world AABB とする。
/// @note LOD は細かいものから粗いものへ cellSizeWorld の昇順で格納する。
struct GpuGeometryCandidate {
    std::array<float, 4> boundsMin{};
    std::array<float, 4> boundsMax{};
    uint32_t lodCount = 0;
    std::array<uint32_t, 3> padding{};
    std::array<GpuGeometryLodRange, kGpuGeometryMaxLods> lods{};
};
static_assert(sizeof(GpuGeometryCandidate) == 176);

/// @brief GPU 選択に使うビュー情報。
/// @note Frustum plane は内側が dot(plane.xyz, point) + plane.w >= 0 で、法線は正規化済みとする。
/// @note projectionScalePixels は透視投影の viewportHeight / (2 * tan(verticalFov / 2))。
/// @note 平行投影は orthographicPixelsPerWorld を使い、距離に依存しない。
struct GpuGeometrySelectionView {
    std::array<std::array<float, 4>, 6> frustumPlanes{};
    std::array<float, 4> cameraAndProjectionScale{};
    std::array<float, 4> projectionMetrics{};
    uint32_t candidateCount = 0;
    std::array<uint32_t, 3> padding{};
};
static_assert(sizeof(GpuGeometrySelectionView) == 144);

/// @brief GPU が 1 候補ごとに書く選択結果。
/// @note visible は 0 / 1、lodIndex は入力候補内の index である。
struct GpuGeometryDecision {
    uint32_t visible = 0;
    uint32_t lodIndex = 0;
    uint32_t indexCount = 0;
    uint32_t candidateIndex = 0;
};
static_assert(sizeof(GpuGeometryDecision) == 16);

/// @brief 1 uint root constant と D3D12 DRAW_INDEXED_ARGUMENTS を連結した間接描画レコード。
/// @note instanceOffset は SV_InstanceID から独立した b15 root constant へ渡す。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_draw_indexed_arguments - member order and sizes.
/// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_indirect_argument_type - CONSTANT command arguments.
/// @see https://microsoft.github.io/hlsl-specs/proposals/0015-extended-command-info/ - system-generated instance identifier semantics.
struct GpuGeometryIndexedArguments {
    uint32_t instanceOffset = 0;
    uint32_t indexCountPerInstance = 0;
    uint32_t instanceCount = 0;
    uint32_t startIndexLocation = 0;
    int32_t baseVertexLocation = 0;
    uint32_t startInstanceLocation = 0;
};
static_assert(sizeof(GpuGeometryIndexedArguments) == kGpuGeometryIndexedArgumentStride);

/// @brief Prepare が Dispatch した候補数と GPU 出力を返す。
/// @note Handles はこの service の frame pool が所有し、GPU が参照中の frame を越えて保持される。
struct GpuGeometryDispatch {
    ResourceHandle<StructuredBufferTag> decisions{};
    ResourceHandle<StructuredBufferTag> indirectArguments{};
    uint32_t candidateCount = 0;
    uint64_t indirectArgumentOffset = 0;
};

/// @brief GPU 可視判定・投影セル幅 LOD 選択・DrawIndexedIndirect 引数生成を行う。
/// @note 透明候補の順序を保ち、視錐台外候補は instanceCount=0 の引数を残す。
/// @note 間接出力は `{instanceOffset, DRAW_INDEXED_ARGUMENTS}` の 24 byte。DrawCall は indirectInstanceOffset を有効にする。
/// @note CPU への結果読み戻しは行わない。LOD の投影密度は任意メッシュの幾何誤差を保証しない。
class GpuGeometrySelection {
public:
    GpuGeometrySelection();
    ~GpuGeometrySelection();
    GpuGeometrySelection(const GpuGeometrySelection&) = delete;
    GpuGeometrySelection& operator=(const GpuGeometrySelection&) = delete;

    /// @brief 1 つの view/batch の候補を検査し、GPU へ選択と間接引数生成を記録する。
    /// @return Dispatch を記録できた場合は true。入力・容量・backend・資源が無効なら false。
    /// @param candidates 同一 VB / IB batch の候補。配列順を引数出力にも維持する。
    /// @param indexCapacityElements 共有 index buffer の有効 index 数。
    /// @param vertexCapacityElements 共有 vertex buffer の有効 vertex 数。
    /// @param out 成功時だけ更新する Dispatch 出力。
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing - ExecuteIndirect argument records.
    [[nodiscard]] bool Prepare(IRenderer& renderer, ResourceManager& resources,
        const GpuGeometrySelectionView& view, std::span<const GpuGeometryCandidate> candidates,
        uint32_t indexCapacityElements, uint32_t vertexCapacityElements, GpuGeometryDispatch& out);

    /// @brief service が保持する GPU buffer pool を解放する。
    void Release(ResourceManager& resources);

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} /// @note namespace fbzz::renderer
