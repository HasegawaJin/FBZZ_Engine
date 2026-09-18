/// @file    Gizmo.hpp
/// @brief   敵 AI / ゲームプレイデバッグ向け高レベルワイヤーフレーム描画ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-06-02
///
/// @note DebugDraw を束ねて「視野錐・ウェイポイント経路・検知範囲・ターゲットライン」を提供する。
///       呼び出し前に DebugDraw::BeginFrame() が呼ばれている必要がある (Flush 区間内で使うこと)。
/// @note 複合プリミティブを各スクリプトが毎回組み立てると重複するため、よく使うパターンを
///       エンジン標準として Script / Editor 双方から呼べるように提供する。
#pragma once

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <span>

namespace fbzz::renderer {

class IRenderer;

class Gizmo {
public:
    /// @name 汎用
    /// @{

    /// @brief XYZ 座標軸を 3 色の矢印で描く (X=赤, Y=緑, Z=青)。
    /// @note GameObject の向きと位置を視覚確認する最頻出パターンをメソッド 1 本で提供する。
    static void TransformAxes(IRenderer& r,
                               const math::Vector3&    position,
                               const math::Quaternion& rotation,
                               float size = 1.0f);
    /// @}

    /// @name AI デバッグ
    /// @{

    /// @brief 視野錐 (SightCone)。AI が "見える" 扇形領域をワイヤーフレームで可視化する。
    /// @note forward 方向を中心に fovDegrees の半角で広がる弧 + 両端から原点への辺を描く。弧は
    ///       水平面 (forward と world-up で張る平面) に描かれ、上下の視角は考慮しない。
    /// @param forward 正規化済みの注視方向。
    /// @param fovDegrees 視野角の合計 (例: 90 → 左右各 45°)。
    /// @param range 最大検知距離。
    static void SightCone(IRenderer& r,
                           const math::Vector3& position,
                           const math::Vector3& forward,
                           float fovDegrees,
                           float range,
                           const math::Vector4& color = { 1.0f, 1.0f, 0.0f, 1.0f });

    /// @brief ウェイポイント経路 (WaypointPath)。巡回ルートを矢印の連鎖で描く。
    /// @note waypoints[i] → waypoints[i+1] を矢印で結び、loop=true なら末尾→先頭も追加する。
    static void WaypointPath(IRenderer& r,
                              std::span<const math::Vector3> waypoints,
                              bool loop = false,
                              const math::Vector4& color = { 0.0f, 1.0f, 1.0f, 1.0f });

    /// @brief 検知範囲リング (DetectionRange)。距離ゾーン別の検知範囲を同心球で描く。
    /// @note innerRadius (例: 攻撃範囲・赤) と outerRadius (例: 索敵範囲・黄) を別色で描く。
    static void DetectionRange(IRenderer& r,
                                const math::Vector3& center,
                                float innerRadius,
                                float outerRadius,
                                const math::Vector4& innerColor = { 1.0f, 0.3f, 0.3f, 1.0f },
                                const math::Vector4& outerColor = { 1.0f, 1.0f, 0.0f, 1.0f });

    /// @brief ターゲットライン (TargetLine)。AI から現在ロックオン中の対象へ矢印を引く。
    /// @note 赤系の矢印 + 対象位置に小球を描いて「どこを狙っているか」を示す。
    static void TargetLine(IRenderer& r,
                            const math::Vector3& from,
                            const math::Vector3& to,
                            const math::Vector4& color = { 1.0f, 0.2f, 0.2f, 1.0f });
    /// @}
};

} // namespace fbzz::renderer
