/// @file    AccelerationStructure.hpp
/// @brief   Backend-independent immutable triangle BLAS and instance TLAS contracts.
/// @author  Hasegawa Jin
/// @date    2026-09-30
#pragma once
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <Math/LinearAlgebra/Matrix4.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace fbzz::renderer {
class PreparedSkinningTicket;

enum class AccelerationStructureKind : uint8_t {
    BOTTOM_LEVEL,
    TOP_LEVEL,
};

/// @note Positions are float3 in object space; indices are uint32 relative to firstVertex.
/// @pre Position components must be finite; indices must be less than vertexCount.
/// @note Non-indexed geometry uses vertexCount / 3 triangles. Opaque skips candidate alpha testing.
struct RayTriangleGeometry {
    ResourceHandle<BufferTag> vertices; ///< 頂点レコードを読み取る非所有バッファハンドル。変形済み入力ではチケットが頂点を供給できる。
    ResourceHandle<BufferTag> indices; ///< `uint32_t` のインデックス列を読む非所有ハンドル。無効なら非インデックス形状。
    uint32_t firstVertex = 0; ///< 頂点バッファ内でこの形状が始まる頂点の添字。
    uint32_t vertexCount = 0; ///< `firstVertex` から続く、形状が参照できる頂点の数。
    uint32_t positionOffset = 0; ///< 各頂点レコードの先頭からローカル位置までのオフセット [byte]。
    uint32_t firstIndex = 0; ///< インデックスバッファ内でこの形状が始まるインデックスの添字。
    uint32_t indexCount = 0; ///< `firstIndex` から続くこの形状のインデックス数。
    bool opaque = true; ///< 三角形候補のアルファ被覆判定を省略できるか。
    /// @note 捕捉済み変形を使う BLAS は native 頂点資源を build frame の完了まで所有する。
    std::shared_ptr<const PreparedSkinningTicket> preparedSkinning; ///< BLAS を作るフレームの完了まで変形頂点資源を保持する共有チケット。
    uint32_t preparedSkinningRow = 0; ///< `preparedSkinning` 内で使用する出力行の添字。
};

/// @note transform uses Math's column-vector convention and must be finite, affine and invertible.
/// @note instanceId is a dense table index limited to 24 bits; mask contains ray purposes, not Engine layers.
/// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_raytracing_instance_desc DXR instance layout
struct RayInstance {
    ResourceHandle<AccelerationStructureTag> bottomLevel; ///< このインスタンスが参照する、構築済み BLAS の非所有ハンドル。
    math::Matrix4 transform = math::Matrix4::Identity(); ///< 形状のローカル空間からワールド空間へのアフィン変換。
    uint32_t instanceId = 0; ///< シェーダーのヒット表を引くための 24 bit の密な添字。
    uint8_t mask = 0xFF; ///< このインスタンスに当てられるレイ用途のビット集合。
    bool doubleSided = false; ///< 表裏を区別した三角形カリングを無効にするか。
    bool frontCounterClockwise = false; ///< 反時計回りの三角形を表として扱うか。
};

/// @note Exactly one nonempty input array must match kind. Inputs are copied at creation.
/// @note Static structures prefer tracing speed; update and compaction are not supported yet.
struct AccelerationStructureDesc {
    AccelerationStructureKind kind = AccelerationStructureKind::BOTTOM_LEVEL; ///< 三角形の BLAS とインスタンスの TLAS のどちらを構築するか。
    std::vector<RayTriangleGeometry> geometries; ///< BLAS の構築に入力する三角形ジオメトリの列。
    std::vector<RayInstance> instances; ///< TLAS の構築に入力する BLAS インスタンスの列。
};

class IAccelerationStructure {
public:
    virtual ~IAccelerationStructure() = default;
    [[nodiscard]] virtual AccelerationStructureKind GetKind() const = 0;
    [[nodiscard]] virtual size_t GetSize() const = 0;
    /// @note True after a successful build recording; completion follows the renderer's queue order.
    [[nodiscard]] virtual bool IsBuilt() const = 0;
    /// @return Only a built TLAS exposes a shader-readable descriptor; otherwise UINT32_MAX.
    [[nodiscard]] virtual uint32_t GetBindlessIndex() const = 0;
};

}
