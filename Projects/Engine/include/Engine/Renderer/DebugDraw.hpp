/// @file    DebugDraw.hpp
/// @brief   ワイヤーフレームのデバッグ描画ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once

#include "IRenderer.hpp"
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Math/Matrix4.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fbzz::renderer {

class ResourceManager;

/// @brief デバッグ描画 1 頂点。DebugDraw.hlsl の頂点入力と一致させること。
struct DebugDrawVertex {
    math::Vector3 position;
    math::Vector4 color;
};

/// @brief Replay で流し直す層。
enum class DebugDrawLayer : uint8_t {
    Overlay     = 1, ///< 深度なしの線
    DepthTested = 2, ///< 深度テストありの線と塗りつぶし三角形
    All         = 3,
};

/// @brief BeginCapture〜EndCapture の間に積まれた頂点。GPU へは出していない。
struct DebugDrawCapture {
    std::vector<DebugDrawVertex> overlayLines;
    std::vector<DebugDrawVertex> depthLines;
    std::vector<DebugDrawVertex> triangles;

    void Clear() { overlayLines.clear(); depthLines.clear(); triangles.clear(); }
    [[nodiscard]] bool Empty() const
    {
        return overlayLines.empty() && depthLines.empty() && triangles.empty();
    }
};

/// @brief 線分・箱・球などをフレーム内バッチへ集め、Flush で LINE_LIST としてまとめて描く。
/// @note 描画命令は BeginFrame〜Flush の区間 (または BeginCapture〜EndCapture) でだけ有効。
/// @note バッチが満杯になると自動で Flush してから積む。呼び出し側の見積もりは不要。
class DebugDraw {
public:
    /// @brief リソースを遅延初期化し、VP 行列を設定してバッチを空にする。深度テストは off に戻る。
    static void BeginFrame(IRenderer& r, ResourceManager& resources, const math::Matrix4& viewProjection);
    /// @brief 蓄積した頂点を Submit してバッチを空にする。1 フレームに何度呼んでもよい。
    /// @note Flush ごとに別の頂点バッファを借りる (DX12 は Submit が記録なので上書きすると壊れる)。
    static void Flush();

    /// @brief 以降の描画命令を GPU へ出さず out へ記録する。上限なし。
    /// @note 別ビューのパス実行中に呼ばないこと (バッチ先が差し替わる)。
    static void BeginCapture(DebugDrawCapture& out);
    static void EndCapture();
    /// @brief 記録した頂点のうち layers をバッチへ積み直す。BeginFrame の区間内で呼ぶ。
    static void Replay(IRenderer& r, const DebugDrawCapture& capture, DebugDrawLayer layers);

    /// @brief true の間、線を深度テストありのバッチへ積む。
    static void SetDepthTest(bool enabled);
    [[nodiscard]] static bool IsDepthTest();

    [[nodiscard]] static size_t PendingLineVertices();
    [[nodiscard]] static size_t PendingTriangleVertices();
    [[nodiscard]] static size_t MaxBatchVertices();

    static void Line(IRenderer& r, const math::Vector3& from, const math::Vector3& to,
                     const math::Vector4& color = {1,1,1,1});
    /// @brief 深度テストありの線分。グリッドのような «世界に置かれた線» 用。
    static void LineDepthTested(IRenderer& r, const math::Vector3& from, const math::Vector3& to,
                                const math::Vector4& color = {1,1,1,1});

    /// @brief 破線。同じ形の実線と重なっても両方読めるようにする線種。
    /// @param dashLength 実線 1 本ぶんのワールド長の目安 [m]。
    /// @param maxDashes  1 線分あたりの分割上限。
    static void LineDashed(IRenderer& r, const math::Vector3& from, const math::Vector3& to,
                           const math::Vector4& color = {1,1,1,1},
                           float dashLength = 0.06f, int maxDashes = 8);
    [[nodiscard]] static size_t DashedLineMaxVertices(int maxDashes = 8);

    /// @brief 折れ線。closed で末尾→先頭も結ぶ。
    static void Polyline(IRenderer& r, std::span<const math::Vector3> points, bool closed = false,
                         const math::Vector4& color = {1,1,1,1});

    static void Box(IRenderer& r, const math::Vector3& center, const math::Vector3& halfExtents,
                    const math::Vector4& color = {0,1,0,1});
    static void Box(IRenderer& r, const math::Vector3& center, const math::Vector3& halfExtents,
                    const math::Quaternion& rotation, const math::Vector4& color = {0,1,0,1});
    static void Sphere(IRenderer& r, const math::Vector3& center, float radius,
                       const math::Vector4& color = {0,1,0,1});
    /// @param halfHeight 両端の半球中心までの距離 (半球は含まない)。軸はローカル Y。
    static void Capsule(IRenderer& r, const math::Vector3& center, float radius, float halfHeight,
                        const math::Vector4& color = {0,1,0,1});
    static void Capsule(IRenderer& r, const math::Vector3& center, float radius, float halfHeight,
                        const math::Quaternion& rotation, const math::Vector4& color = {0,1,0,1});

    /// @brief normal に直交する平面上の円。
    /// @param normal 長さ 0 のときは描かない。
    static void Circle(IRenderer& r, const math::Vector3& center, const math::Vector3& normal,
                       float radius, const math::Vector4& color = {1,1,1,1});
    /// @brief normal まわりに fromDirection から angleRadians だけ回る円弧 (右手系で正方向)。
    /// @param fromDirection normal に射影して使う。平行なら描かない。
    static void Arc(IRenderer& r, const math::Vector3& center, const math::Vector3& normal,
                    const math::Vector3& fromDirection, float radius, float angleRadians,
                    const math::Vector4& color = {1,1,1,1});

    /// @param headLength ヘッドのワールド長。シャフトを超えるとクランプする。
    static void Arrow(IRenderer& r,
                      const math::Vector3& from, const math::Vector3& to,
                      float headLength = 0.2f, float headRadius = 0.05f,
                      const math::Vector4& color = {1,1,0,1});

    /// @param direction 頂点から底面への向き。正規化不要、長さ 0 なら描かない。
    static void Cone(IRenderer& r,
                     const math::Vector3& apex, const math::Vector3& direction,
                     float height, float baseRadius,
                     const math::Vector4& color = {1,1,0,1});

    /// @brief 凸ポリゴンを半透明で塗る (fan 分割・深度テストあり)。
    static void FilledPolygon(IRenderer& r, const math::Vector3* vertices, size_t count,
                              const math::Vector4& color = {0.12f, 0.35f, 0.90f, 0.30f});
};

} // namespace fbzz::renderer
