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
    /// @note 深度バッファを「何も描かれていない」状態 (無限遠) で初期化する。
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
    /// @note ① ビュー空間に変換して線形深度を得る
    const math::Vector4 viewPos = m_view * math::Vector4{ worldPos.x, worldPos.y, worldPos.z, 1.0f };

    /// @note DirectX 左手系: カメラ前方が +Z → viewPos.z が視線深度
    /// @note カメラ後方・near 平面より手前
    if (viewPos.z <= m_nearZ) return false;

    outLinearDepth = (viewPos.z - m_nearZ) / (m_farZ - m_nearZ);
    outLinearDepth = std::clamp(outLinearDepth, 0.0f, 1.0f);

    /// @note ② 射影変換でクリップ空間へ
    const math::Vector4 clipPos = m_proj * viewPos;
    if (clipPos.w <= 0.0f) return false;

    const float ndcX = clipPos.x / clipPos.w;
    const float ndcY = clipPos.y / clipPos.w;

    /// @note ③ NDC (-1,1) → スクリーン座標 [0, kWidth) x [0, kHeight)
    ///       Y は上下反転 (DirectX スクリーン座標の Y は下が正)
    outSx = (ndcX + 1.0f) * 0.5f * static_cast<float>(kWidth);
    outSy = (1.0f - ndcY) * 0.5f * static_cast<float>(kHeight);

    return true;
}

bool OcclusionCuller::TestAndRaster(const math::Vector3& worldCenter, float worldRadius,
                                    bool registerAsOccluder)
{
    /// @note バウンディング球の半径が 0 の場合はカリング未対応メッシュ → 描画する
    if (worldRadius <= 0.0f) return true;

    /// @name ビュー空間への変換
    const math::Vector4 viewCenter4 = m_view * math::Vector4{ worldCenter.x, worldCenter.y, worldCenter.z, 1.0f };
    const float viewZ = viewCenter4.z;

    /// @note カメラ後方・near 平面よりも球全体が前にある場合は常に可視
    /// @note 完全に後方
    if (viewZ + worldRadius <= m_nearZ) return true;
    /// @note near をまたぐ → 保守的に可視
    if (viewZ - worldRadius <= 0.0f)    return true;

    /// @name スクリーン座標への射影
    float centerSx, centerSy, centerLinDepth;
    if (!ProjectPoint(worldCenter, centerSx, centerSy, centerLinDepth)) return true;

    /// @note ビュー空間でのスクリーン半径の近似式。射影行列の m[0][0]/m[1][1] は cotangent(fov/2) 相当で、これで除して正規化スクリーン空間の半径を得る。
    /// @note 正投影では距離で縮まないため 1/viewZ を掛けない。掛けると引きの大きい正投影カメラで半径が数百分の1になり、見えているものが消える。
    const float radiusScale = m_orthographic ? 1.0f : 1.0f / viewZ;
    const float ndcRx = worldRadius * radiusScale * m_proj.m[0][0];
    const float ndcRy = worldRadius * radiusScale * m_proj.m[1][1];
    const float screenRx = ndcRx * static_cast<float>(kWidth)  * 0.5f;
    const float screenRy = ndcRy * static_cast<float>(kHeight) * 0.5f;

    /// @note スクリーン上の AABB を計算する
    const int x0 = std::max(0,         static_cast<int>(centerSx - screenRx));
    const int y0 = std::max(0,         static_cast<int>(centerSy - screenRy));
    const int x1 = std::min(kWidth  - 1, static_cast<int>(centerSx + screenRx));
    const int y1 = std::min(kHeight - 1, static_cast<int>(centerSy + screenRy));

    /// @note スクリーン外に完全に出た場合はフラスタムカリングで処理されているはずだが念のため可視扱い
    if (x0 > x1 || y0 > y1) return true;

    /// @name 球の最近点のリニア深度
    /// @note 最近点 = ビュー Z 軸方向での球の前面 (viewZ - radius)
    const float nearFaceZ = viewZ - worldRadius;
    const float nearLinDepth = std::clamp(
        (nearFaceZ - m_nearZ) / (m_farZ - m_nearZ), 0.0f, 1.0f);

    /// @name オクルージョンテスト
    /// @note リニア深度は小さいほどカメラに近い。対象ピクセル全てで既存深度が球の最近点 (nearLinDepth) より手前なら、より近い不透明サーフェスが既にあり球全体は隠蔽されている。
    bool isOccluded = true;
    for (int y = y0; y <= y1 && isOccluded; ++y) {
        for (int x = x0; x <= x1 && isOccluded; ++x) {
            if (m_depth[y * kWidth + x] >= nearLinDepth) {
                /// @note このピクセルでは球が可視
                isOccluded = false;
            }
        }
    }

    /// @note 完全隠蔽確定
    if (isOccluded) return false;

    /// @note 球で近似してよい形だけを遮蔽者にする。薄い形はここで打ち切り、
    ///       「隠される側」としてだけ扱う (判定はもう終わっているので可視で返す)。
    if (!registerAsOccluder) return true;

    /// @name オクルーダー登録
    /// @note 可視だったので、この球を「以降のオブジェクトを遮るもの」として深度バッファへ焼く。保守側に倒す: 被覆は投影円に内接する軸平行正方形 (半径×1/√2) のみ、深度は球の背面 (viewZ + radius) を使う。
    /// @note 球に内包されるメッシュ表面は必ず背面より手前にあるため、この深度で遮蔽できるものは実メッシュでも確実に遮蔽できる。前面深度を AABB 全域に書くと、実際にはメッシュが無い位置まで遮蔽扱いになり見えているオブジェクトが消える。
    /// @note バウンディング球はメッシュ形状の近似でしかなく、これ以上の精度には専用の低ポリ occluder メッシュのラスタライズが要る。
    /// @note 1/√2
    constexpr float kInscribedScale = 0.70710678f;
    const float innerRx = screenRx * kInscribedScale;
    const float innerRy = screenRy * kInscribedScale;

    const int ox0 = std::max(0,           static_cast<int>(std::ceil (centerSx - innerRx)));
    const int oy0 = std::max(0,           static_cast<int>(std::ceil (centerSy - innerRy)));
    const int ox1 = std::min(kWidth  - 1, static_cast<int>(std::floor(centerSx + innerRx)));
    const int oy1 = std::min(kHeight - 1, static_cast<int>(std::floor(centerSy + innerRy)));
    /// @note 内接領域が 1 ピクセルにも満たない → 遮蔽者にしない
    if (ox0 > ox1 || oy0 > oy1) return true;

    const float farFaceZ = viewZ + worldRadius;
    const float backLinDepth = std::clamp(
        (farFaceZ - m_nearZ) / (m_farZ - m_nearZ), 0.0f, 1.0f);

    for (int y = oy0; y <= oy1; ++y) {
        for (int x = ox0; x <= ox1; ++x) {
            float& cell = m_depth[y * kWidth + x];
            if (backLinDepth < cell) cell = backLinDepth;
        }
    }

    /// @note 可視
    return true;
}

} // namespace fbzz::scene
