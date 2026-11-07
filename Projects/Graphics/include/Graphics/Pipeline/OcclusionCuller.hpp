/// @file    OcclusionCuller.hpp
/// @brief   CPU ソフトウェアオクルージョンカリング。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Renderer/Camera.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <array>
#include <cstdint>
#include <span>

namespace fbzz::renderer {

/// @note 同じビュー内の CPU 深度を使い、過去フレームの可視性には依存しない。
class OcclusionCuller {
public:
    static constexpr int kWidth = 256;
    static constexpr int kHeight = 144;
    static constexpr size_t kGpuRows = 9 + kWidth * kHeight / 4;

    FBZZ_GRAPHICS_API OcclusionCuller();
    /// @note jitter は描画と同じ NDC オフセット。Reset ごとに過去の深度を破棄する。
    FBZZ_GRAPHICS_API void Reset(const Camera& cam, float jitterX = 0, float jitterY = 0);
    /// @note 同じインスタンスが別ビューで初期化された場合、準備済み CPU/GPU 遮蔽を失効させる。
    [[nodiscard]] uint64_t GetResetVersion() const { return m_resetVersion; }
    /// @param opaqueInteriorRadius 同じ中心の実形状内部に収まる球 [m]。不透明被覆が確定した呼び出しだけ正値を渡す。
    /// @note 外接球は検査だけに使い、遮蔽者は内接球の投影でセル全体を覆う領域だけ登録する。
    /// @return 不明・near 交差・数値不正は true。false は外接球全体が既存の不透明面に隠れる場合だけ。
    FBZZ_GRAPHICS_API bool TestAndRaster(const math::Vector3& worldCenter, float worldRadius,
        float opaqueInteriorRadius = 0);
    /// @note 頂点はワールド空間。near をまたぐ面・裏面は登録せず、セル全体の被覆と最遠深度だけを使う。
    FBZZ_GRAPHICS_API void RasterTriangle(const math::Vector3& a, const math::Vector3& b, const math::Vector3& c);
    /// @note 共有対角線を持つ 2 面の凸投影だけを結合する。凹・折り返しは三角形単位へ戻す。
    FBZZ_GRAPHICS_API void RasterQuad(const math::Vector3& a, const math::Vector3& b,
        const math::Vector3& c, const math::Vector3& d);
    /// @note 既存深度に隠れる箱を省き、選んだ面の頂点投影を共有する。mask は Z-/Z+/X-/X+/Y-/Y+ の順。
    FBZZ_GRAPHICS_API void RasterBox(const math::Vector3& low, const math::Vector3& high, uint8_t faceMask = 0x3f);
    /// @note ワールド AABB の全頂点で検査し、薄い水面を外接球へ膨らませない。near 交差・不正境界は可視。
    [[nodiscard]] FBZZ_GRAPHICS_API bool TestAabb(const math::Vector3& low, const math::Vector3& high) const;
    /// @note 同一ビューの GPU 検査用。先頭 8 行は view/projection、9 行目は near/far/寸法、以降は線形深度。
    FBZZ_GRAPHICS_API void CopyGpuRows(std::span<math::Vector4> rows) const;

private:
    bool RasterPolygon(std::span<const math::Vector3> vertices);
    float m_depth[kWidth * kHeight];
    math::Matrix4 m_view;
    math::Matrix4 m_proj;
    float m_nearZ = 0.1f;
    float m_farZ = 1000.0f;
    bool m_orthographic = false;
    bool m_valid = false;
    bool m_hasDepth = false;
    uint64_t m_resetVersion = 0;
};

} /// @note namespace fbzz::renderer
