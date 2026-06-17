// FBZZ Engine
// OcclusionCuller.hpp | fbzz::scene
// CPU ソフトウェアオクルージョンカリング
//
// 概要:
//   128x72 のリニア深度バッファを CPU 上に持ち、ワールド空間のバウンディング球を
//   スクリーン空間に投影して可視判定とオクルーダー登録を行う。
//
// WHY: GPU Occlusion Query は結果が 1 フレーム遅延するため、高速移動シーンで
//      誤カリングが起きやすい。CPU SW 方式は即時結果かつ実装が単純。
#pragma once

#include <Engine/Renderer/Camera.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

class OcclusionCuller {
public:
    static constexpr int kWidth  = 128;
    static constexpr int kHeight = 72;

    OcclusionCuller();

    // Reset — フレーム開始時にカメラ行列を記録し、深度バッファを 1.0 で初期化する。
    void Reset(const renderer::Camera& cam);

    // TestAndRaster — バウンディング球を可視判定し、可視ならオクルーダーとして登録する。
    bool TestAndRaster(const math::Vector3& worldCenter, float worldRadius);

private:
    float m_depth[kWidth * kHeight];

    math::Matrix4 m_view;
    math::Matrix4 m_proj;
    float         m_nearZ = 0.1f;
    float         m_farZ  = 1000.0f;

    // ProjectPoint — ワールド座標を低解像度深度バッファ上のスクリーン座標へ射影する。
    bool ProjectPoint(const math::Vector3& worldPos,
                      float& outSx, float& outSy,
                      float& outLinearDepth) const;
};

} // namespace fbzz::scene
