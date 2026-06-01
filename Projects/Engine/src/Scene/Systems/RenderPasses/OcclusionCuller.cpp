// FBZZ Engine
// OcclusionCuller.cpp | fbzz::scene
// CPU ソフトウェアオクルージョンカリングの実装
#include <Engine/Scene/Systems/OcclusionCuller.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {

OcclusionCuller::OcclusionCuller()
{
    // 深度バッファを「何も描かれていない」状態 (無限遠) で初期化する。
    std::fill(std::begin(m_depth), std::end(m_depth), 1.0f);
}

void OcclusionCuller::Reset(const renderer::Camera& cam)
{
    m_view  = cam.GetViewMatrix();
    m_proj  = cam.GetProjectionMatrix();
    m_nearZ = cam.m_near;
    m_farZ  = cam.m_far;
    std::fill(std::begin(m_depth), std::end(m_depth), 1.0f);
}

bool OcclusionCuller::ProjectPoint(const math::Vector3& worldPos,
                                    float& outSx, float& outSy,
                                    float& outLinearDepth) const
{
    // ① ビュー空間に変換して線形深度を得る
    const math::Vector4 viewPos = m_view * math::Vector4{ worldPos.x, worldPos.y, worldPos.z, 1.0f };

    // DirectX 左手系: カメラ前方が +Z → viewPos.z が視線深度
    if (viewPos.z <= m_nearZ) return false; // カメラ後方・near 平面より手前

    outLinearDepth = (viewPos.z - m_nearZ) / (m_farZ - m_nearZ);
    outLinearDepth = std::clamp(outLinearDepth, 0.0f, 1.0f);

    // ② 射影変換でクリップ空間へ
    const math::Vector4 clipPos = m_proj * viewPos;
    if (clipPos.w <= 0.0f) return false;

    const float ndcX = clipPos.x / clipPos.w;
    const float ndcY = clipPos.y / clipPos.w;

    // ③ NDC (-1,1) → スクリーン座標 [0, kWidth) x [0, kHeight)
    // Y は上下反転 (DirectX スクリーン座標の Y は下が正)
    outSx = (ndcX + 1.0f) * 0.5f * static_cast<float>(kWidth);
    outSy = (1.0f - ndcY) * 0.5f * static_cast<float>(kHeight);

    return true;
}

bool OcclusionCuller::TestAndRaster(const math::Vector3& worldCenter, float worldRadius)
{
    // バウンディング球の半径が 0 の場合はカリング未対応メッシュ → 描画する
    if (worldRadius <= 0.0f) return true;

    // ── ビュー空間への変換 ────────────────────────────────────────────────────
    const math::Vector4 viewCenter4 = m_view * math::Vector4{ worldCenter.x, worldCenter.y, worldCenter.z, 1.0f };
    const float viewZ = viewCenter4.z;

    // カメラ後方・near 平面よりも球全体が前にある場合は常に可視
    if (viewZ + worldRadius <= m_nearZ) return true; // 完全に後方
    if (viewZ - worldRadius <= 0.0f)    return true; // near をまたぐ → 保守的に可視

    // ── スクリーン座標への射影 ─────────────────────────────────────────────────
    float centerSx, centerSy, centerLinDepth;
    if (!ProjectPoint(worldCenter, centerSx, centerSy, centerLinDepth)) return true;

    // ビュー空間でのスクリーン半径 (近似式)
    // WHY: 射影行列の m[0][0] / m[1][1] はそれぞれ cotangent(fovX/2) / cotangent(fovY/2) に相当し、
    //      除することで正規化スクリーン空間の半径を求められる。厳密ではないが精度は十分。
    const float ndcRx = worldRadius / viewZ * m_proj.m[0][0];
    const float ndcRy = worldRadius / viewZ * m_proj.m[1][1];
    const float screenRx = ndcRx * static_cast<float>(kWidth)  * 0.5f;
    const float screenRy = ndcRy * static_cast<float>(kHeight) * 0.5f;

    // スクリーン上の AABB を計算する
    const int x0 = std::max(0,         static_cast<int>(centerSx - screenRx));
    const int y0 = std::max(0,         static_cast<int>(centerSy - screenRy));
    const int x1 = std::min(kWidth  - 1, static_cast<int>(centerSx + screenRx));
    const int y1 = std::min(kHeight - 1, static_cast<int>(centerSy + screenRy));

    // スクリーン外に完全に出た場合はフラスタムカリングで処理されているはずだが念のため可視扱い
    if (x0 > x1 || y0 > y1) return true;

    // ── 球の最近点のリニア深度 ────────────────────────────────────────────────
    // 最近点 = ビュー Z 軸方向での球の前面 (viewZ - radius)
    const float nearFaceZ = viewZ - worldRadius;
    const float nearLinDepth = std::clamp(
        (nearFaceZ - m_nearZ) / (m_farZ - m_nearZ), 0.0f, 1.0f);

    // ── オクルージョンテスト ──────────────────────────────────────────────────
    // 深度バッファ上の対象ピクセルすべてで既存の深度が球の最近点より手前 (小さい) なら
    // 球全体が隠蔽されている → カリング
    //
    // WHY: リニア深度では「小さい値 = カメラに近い」。
    //      depth_buffer[pixel] < nearLinDepth が成立する場合、
    //      そのピクセルにはより近い不透明サーフェスが既に記録されており、
    //      球の最近点よりも手前に存在するため球を遮蔽できる。
    bool isOccluded = true;
    for (int y = y0; y <= y1 && isOccluded; ++y) {
        for (int x = x0; x <= x1 && isOccluded; ++x) {
            if (m_depth[y * kWidth + x] >= nearLinDepth) {
                isOccluded = false; // このピクセルでは球が可視
            }
        }
    }

    if (isOccluded) return false; // 完全隠蔽確定

    // ── オクルーダー登録 ──────────────────────────────────────────────────────
    // 球が可視なので、球の前面深度を深度バッファに書き込む。
    // 以降の「より奥にある」球はここで遮られてカリングされる。
    const float frontLinDepth = nearLinDepth;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            float& cell = m_depth[y * kWidth + x];
            if (frontLinDepth < cell) cell = frontLinDepth;
        }
    }

    return true; // 可視
}

} // namespace fbzz::scene
