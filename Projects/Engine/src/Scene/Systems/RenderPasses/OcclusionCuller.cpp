/// @file    OcclusionCuller.cpp
/// @brief   CPU ソフトウェアオクルージョンカリングの実装。
/// @author  Hasegawa Jin
/// @date    2026-05-31
#include <Engine/Scene/Systems/RenderPasses/OcclusionCuller.hpp>
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
    m_orthographic = cam.m_projection == renderer::ProjectionMode::Orthographic;
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

bool OcclusionCuller::TestAndRaster(const math::Vector3& worldCenter, float worldRadius,
                                    bool registerAsOccluder)
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
    // 平行投影は距離で縮まないので 1/viewZ を掛けない。掛けると引きの大きい
    // 正投影カメラでは半径が数百分の 1 になり、見えているものが軒並み消える。
    const float radiusScale = m_orthographic ? 1.0f : 1.0f / viewZ;
    const float ndcRx = worldRadius * radiusScale * m_proj.m[0][0];
    const float ndcRy = worldRadius * radiusScale * m_proj.m[1][1];
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

    // 球で近似してよい形だけを遮蔽者にする。薄い形はここで打ち切り、
    // 「隠される側」としてだけ扱う (判定はもう終わっているので可視で返す)。
    if (!registerAsOccluder) return true;

    // ── オクルーダー登録 ──────────────────────────────────────────────────────
    // 可視だったので、この球を「以降のオブジェクトを遮るもの」として深度バッファへ焼く。
    //
    // WHY ここが従来バグっていたか:
    //   旧実装は「球の前面深度 (nearFaceZ)」を「スクリーン AABB 全域」へ書いていた。
    //   これは遮蔽者としては両方向に過大申告で、見えているオブジェクトが消える。
    //     1) 深度: 球の前面はメッシュ表面の最も手前の 1 点でしかない。全域をその深度で
    //        埋めると、実際にはメッシュが存在しない (球の縁の) 位置まで「手前に不透明面が
    //        ある」と主張することになる。
    //     2) 被覆: AABB の四隅は球の投影円の外側で、そもそも球すら覆っていない。
    //   結果、大きな柱や地面の隣に立っているだけのオブジェクトが、柱の球の AABB に
    //   重なるというだけで丸ごとカリングされていた。
    //
    // WHAT 直した内容: 遮蔽者は「確実に覆う範囲」を「確実に手前と言える深度」でだけ
    //   主張する、という保守側へ倒す。
    //     - 被覆: 投影円に内接する軸平行正方形 (半径 × 1/√2) のみ。円の外へはみ出さない。
    //     - 深度: 球の背面 (viewZ + radius)。球に内包されるメッシュ表面は必ずこれより手前
    //       にあるので、この深度で遮蔽できるものは実メッシュでも確実に遮蔽できる。
    //   落とせる数は減るが、「見えているのに消える」は原理的に起きなくなる。
    //   NOTE: バウンディング球はメッシュ形状の近似でしかないため、これ以上に効かせるには
    //         専用の低ポリ occluder メッシュをラスタライズする方式が必要になる。
    constexpr float kInscribedScale = 0.70710678f; // 1/√2
    const float innerRx = screenRx * kInscribedScale;
    const float innerRy = screenRy * kInscribedScale;

    const int ox0 = std::max(0,           static_cast<int>(std::ceil (centerSx - innerRx)));
    const int oy0 = std::max(0,           static_cast<int>(std::ceil (centerSy - innerRy)));
    const int ox1 = std::min(kWidth  - 1, static_cast<int>(std::floor(centerSx + innerRx)));
    const int oy1 = std::min(kHeight - 1, static_cast<int>(std::floor(centerSy + innerRy)));
    if (ox0 > ox1 || oy0 > oy1) return true; // 内接領域が 1 ピクセルにも満たない → 遮蔽者にしない

    const float farFaceZ = viewZ + worldRadius;
    const float backLinDepth = std::clamp(
        (farFaceZ - m_nearZ) / (m_farZ - m_nearZ), 0.0f, 1.0f);

    for (int y = oy0; y <= oy1; ++y) {
        for (int x = ox0; x <= ox1; ++x) {
            float& cell = m_depth[y * kWidth + x];
            if (backLinDepth < cell) cell = backLinDepth;
        }
    }

    return true; // 可視
}

} // namespace fbzz::scene
