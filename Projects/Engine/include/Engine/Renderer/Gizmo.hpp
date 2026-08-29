/// @file    Gizmo.hpp
/// @brief   敵 AI / ゲームプレイデバッグ向け高レベルワイヤーフレーム描画ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-06-02
///
/// DebugDraw を束ねて「視野錐・ウェイポイント経路・検知範囲・ターゲットライン」を提供する。
/// 呼び出し前に DebugDraw::BeginFrame() が呼ばれている必要がある (Flush 区間内で使うこと)。
///
/// WHY: DebugDraw は汎用的な低レベル API だが、AI デバッグに必要な複合プリミティブを
/// 各スクリプトが毎回組み立てると重複が生じる。
/// Gizmo はよく使うパターンをエンジン標準として提供し、Script / Editor 双方から呼べるようにする。
#pragma once

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <span>

namespace fbzz::renderer {

class IRenderer;

class Gizmo {
public:
    // ── 汎用 ─────────────────────────────────────────────────────────────────

    // XYZ 座標軸を 3 色の矢印で描く (X=赤, Y=緑, Z=青)。
    // WHY: GameObject の向きと位置を視覚確認する最頻出パターンをメソッド 1 本で提供する。
    static void TransformAxes(IRenderer& r,
                               const math::Vector3&    position,
                               const math::Quaternion& rotation,
                               float size = 1.0f);

    // ── AI デバッグ ──────────────────────────────────────────────────────────

    // 視野錐 (SightCone) — AI が "見える" 扇形領域をワイヤーフレームで可視化する。
    // WHAT: forward 方向を中心に fovDegrees の半角で広がる弧 + 両端から原点への辺を描く。
    //       弧は水平面 (forward と world-up で張る平面) に描かれ、上下の視角は考慮しない。
    // forward: 正規化済みの注視方向
    // fovDegrees: 視野角の合計 (例: 90 → 左右各 45°)
    // range: 最大検知距離
    static void SightCone(IRenderer& r,
                           const math::Vector3& position,
                           const math::Vector3& forward,
                           float fovDegrees,
                           float range,
                           const math::Vector4& color = { 1.0f, 1.0f, 0.0f, 1.0f });

    // ウェイポイント経路 (WaypointPath) — 巡回ルートを矢印の連鎖で描く。
    // WHAT: waypoints[i] → waypoints[i+1] を矢印で結び、loop=true なら末尾→先頭も追加する。
    // WHY: AI が次にどこへ向かうかを一目で確認できるようにする。
    static void WaypointPath(IRenderer& r,
                              std::span<const math::Vector3> waypoints,
                              bool loop = false,
                              const math::Vector4& color = { 0.0f, 1.0f, 1.0f, 1.0f });

    // 検知範囲リング (DetectionRange) — 距離ゾーン別の検知範囲を同心球で描く。
    // WHAT: innerRadius (例: 攻撃範囲・赤) と outerRadius (例: 索敵範囲・黄) を別色で描く。
    //       2 つの球で「近づくと攻撃、遠くても気づく」を視覚化する。
    static void DetectionRange(IRenderer& r,
                                const math::Vector3& center,
                                float innerRadius,
                                float outerRadius,
                                const math::Vector4& innerColor = { 1.0f, 0.3f, 0.3f, 1.0f },
                                const math::Vector4& outerColor = { 1.0f, 1.0f, 0.0f, 1.0f });

    // ターゲットライン (TargetLine) — AI から現在ロックオン中の対象へ矢印を引く。
    // WHAT: 赤系の矢印 + 対象位置に小球を描いて「どこを狙っているか」を示す。
    static void TargetLine(IRenderer& r,
                            const math::Vector3& from,
                            const math::Vector3& to,
                            const math::Vector4& color = { 1.0f, 0.2f, 0.2f, 1.0f });
};

} // namespace fbzz::renderer
