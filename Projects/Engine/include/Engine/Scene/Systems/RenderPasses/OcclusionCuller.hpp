/// @file    OcclusionCuller.hpp
/// @brief   CPU ソフトウェアオクルージョンカリング。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// 128x72 のリニア深度バッファを CPU 上に持ち、ワールド空間のバウンディング球をスクリーン空間
/// に投影して可視判定とオクルーダー登録を行う。
/// @note GPU Occlusion Query は結果が 1 フレーム遅延し、高速移動シーンで誤カリングが起きやすい
///       ため、即時結果で単純な CPU SW 方式にしている。
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

    /// Reset — フレーム開始時にカメラ行列を記録し、深度バッファを 1.0 で初期化する。
    void Reset(const renderer::Camera& cam);

    /// TestAndRaster — バウンディング球を可視判定し、可視ならオクルーダーとして登録する。
    /// registerAsOccluder=false のときは判定だけを行い、深度バッファへは焼かない。
    /// @note 登録は「球の内側は概ねメッシュで埋まっている」前提に立つ。板ポリ・壁パネルのような
    ///       薄い形はこの前提を満たさず、実体の無い空間まで遮蔽を主張して«見えているのに消える»を
    ///       生む。前提を満たすかの判断はメッシュ形状を知る呼び出し側 (IsReliableOccluder) が行う。
    bool TestAndRaster(const math::Vector3& worldCenter, float worldRadius,
                       bool registerAsOccluder = true);

private:
    float m_depth[kWidth * kHeight];

    math::Matrix4 m_view;
    math::Matrix4 m_proj;
    float         m_nearZ = 0.1f;
    float         m_farZ  = 1000.0f;
    /// 平行投影では見かけの大きさが距離に依らない。スクリーン半径の式が変わるため、
    /// 射影の別をフレーム開始時に控えておく。
    bool          m_orthographic = false;

    /// ProjectPoint — ワールド座標を低解像度深度バッファ上のスクリーン座標へ射影する。
    bool ProjectPoint(const math::Vector3& worldPos,
                      float& outSx, float& outSy,
                      float& outLinearDepth) const;
};

} // namespace fbzz::scene
