/// @file    UIRenderClipping.cpp
/// @brief   Canvas のマスクと画像・グリフで共有する凸多角形クリップ。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "UIRendering.hpp"

#include "../../Scene/Systems/UI/UIRuntimeGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace fbzz::scene::ui_render_detail {

/// @note dot(p - point, normal) >= 0 側を残す (Sutherland-Hodgman)。
/// @note 凸多角形の入力に対し、出力は入力 + 1 頂点までしか増えない。
int ClipHalfPlane(const ClipVertex* input, int vertexCount, const math::Vector2& point,
                  const math::Vector2& normal, ClipVertex* output, int outputCapacity)
{
    int outputCount = 0;
    const auto emit = [&](const ClipVertex& vertex) {
        if (outputCount < outputCapacity) output[outputCount++] = vertex;
    };
    for (int vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
        const ClipVertex& startVertex = input[vertexIndex];
        const ClipVertex& endVertex = input[(vertexIndex + 1) % vertexCount];
        const float startDistance = (startVertex.position.x - point.x) * normal.x
            + (startVertex.position.y - point.y) * normal.y;
        const float endDistance = (endVertex.position.x - point.x) * normal.x
            + (endVertex.position.y - point.y) * normal.y;
        if (startDistance >= 0.0f) emit(startVertex);
        if ((startDistance >= 0.0f) != (endDistance >= 0.0f)) {
            const float intersectionWeight = startDistance / (startDistance - endDistance);
            emit({
                { startVertex.position.x + (endVertex.position.x - startVertex.position.x) * intersectionWeight,
                  startVertex.position.y + (endVertex.position.y - startVertex.position.y) * intersectionWeight },
                { startVertex.uv.x + (endVertex.uv.x - startVertex.uv.x) * intersectionWeight,
                  startVertex.uv.y + (endVertex.uv.y - startVertex.uv.y) * intersectionWeight },
                { startVertex.color.x + (endVertex.color.x - startVertex.color.x) * intersectionWeight,
                  startVertex.color.y + (endVertex.color.y - startVertex.color.y) * intersectionWeight,
                  startVertex.color.z + (endVertex.color.z - startVertex.color.z) * intersectionWeight,
                  startVertex.color.w + (endVertex.color.w - startVertex.color.w) * intersectionWeight }
            });
        }
    }
    return outputCount;
}

/// @note いま積まれているクリップ面すべてで多角形を削る。頂点は Canvas 空間で渡すこと。
/// @note 何も残らなければ 0。1 枚ずつ順に当てるのは、共通部分の定義がそのまま
/// @note 「全半平面を満たす点」だから。矩形どうしの交差として畳むと回転で表せなくなる。
int ClipPolygonToPlanes(const std::vector<UIClipPlane>& planes,
                        const ClipVertex* input, int vertexCount,
                        ClipVertex* output, int outputCapacity)
{
    if (planes.empty()) {
        const int copied = (std::min)(vertexCount, outputCapacity);
        for (int vertexIndex = 0; vertexIndex < copied; ++vertexIndex) output[vertexIndex] = input[vertexIndex];
        return copied;
    }
    ClipVertex bufferA[MAX_CLIP_POLYGON_VERTICES];
    ClipVertex bufferB[MAX_CLIP_POLYGON_VERTICES];
    int remainingCount = (std::min)(vertexCount, MAX_CLIP_POLYGON_VERTICES);
    for (int vertexIndex = 0; vertexIndex < remainingCount; ++vertexIndex) bufferA[vertexIndex] = input[vertexIndex];

    ClipVertex* inputBuffer = bufferA;
    ClipVertex* outputBuffer = bufferB;
    for (const UIClipPlane& plane : planes) {
        remainingCount = ClipHalfPlane(inputBuffer, remainingCount, plane.point, plane.normal,
            outputBuffer, MAX_CLIP_POLYGON_VERTICES);
        if (remainingCount < 3) return 0;
        std::swap(inputBuffer, outputBuffer);
    }
    const int copied = (std::min)(remainingCount, outputCapacity);
    for (int vertexIndex = 0; vertexIndex < copied; ++vertexIndex) output[vertexIndex] = inputBuffer[vertexIndex];
    return copied;
}

/// @note 矩形 (回転あり) を内向き 4 面としてクリップスタックへ積む。
/// @note 戻り値は積んだ枚数。抜けるときに同じ数だけ外す。
std::size_t PushClipRect(std::vector<UIClipPlane>& planes, const ui_runtime_detail::Rect& rect, float rotationZ)
{
    const float cosRotation = std::cosf(rotationZ);
    const float sinRotation = std::sinf(rotationZ);
    /// @note 回転の中心は要素の中心。描画側 (LocalToCanvas) と同じ規則で揃える。
    const math::Vector2 center = { rect.pos.x + rect.size.x * 0.5f,
                                   rect.pos.y + rect.size.y * 0.5f };
    /// @note 辺の外向き法線を回した 4 本。内側に残したいので符号を反転して積む。
    /// @note 要素ローカルの +x が Canvas 上で向く方向
    const math::Vector2 axisX = { cosRotation, sinRotation };
    /// @note 同 +y
    const math::Vector2 axisY = { -sinRotation, cosRotation };
    const float halfWidth = rect.size.x * 0.5f;
    const float halfHeight = rect.size.y * 0.5f;

    const auto push = [&](const math::Vector2& normal, float halfExtent) {
        /// @note 面上の 1 点 = 中心から法線と逆向きへ halfExtent 進んだところ。
        planes.push_back({ { center.x - normal.x * halfExtent,
                             center.y - normal.y * halfExtent }, normal });
    };
    /// @note 左辺 (内向き = +x)
    push(axisX, halfWidth);
    /// @note 右辺
    push({ -axisX.x, -axisX.y }, halfWidth);
    /// @note 上辺 (内向き = +y)
    push(axisY, halfHeight);
    /// @note 下辺
    push({ -axisY.x, -axisY.y }, halfHeight);
    return 4;
}

} /// @note namespace fbzz::scene::ui_render_detail
