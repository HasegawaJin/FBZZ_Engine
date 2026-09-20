/// @file    DebugDraw.cpp
/// @brief   ワイヤーフレームのデバッグ描画実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include <Graphics/Renderer/DebugDraw.hpp>
#include <Graphics/Renderer/DebugDrawBatch.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/DynamicBufferPool.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Core/Logger.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fbzz::renderer {

namespace {

struct DebugCamCB {
    math::Matrix4 viewProjection;
};

constexpr uint32_t MAX_DEBUG_VERTICES = 65536;
constexpr int      CIRCLE_SEGMENTS    = 24;
constexpr float    PI                 = 3.14159265358979f;
/// @brief 破線 1 周期のうち実線の割合。狭いと下の線が見えず、広いと破線側の形が読めない。
constexpr float    DASH_DUTY          = 0.55f;

IRenderer*                        s_renderer  = nullptr;
ResourceManager*                  s_resources = nullptr;
ResourceHandle<ShaderTag>         s_shader;
ResourceHandle<ConstantBufferTag> s_cameraCB;
ResourceHandle<PipelineStateTag>  s_pso;
ResourceHandle<PipelineStateTag>  s_depthPso;
ResourceHandle<PipelineStateTag>  s_triPso;
std::vector<DebugDrawVertex>      s_batch;
std::vector<DebugDrawVertex>      s_depthBatch;
std::vector<DebugDrawVertex>      s_triBatch;
bool                              s_depthTest = false;
DebugDrawCapture*                 s_capture   = nullptr;

/// @brief Flush 1 回ぶんの頂点バッファを貸し出すプール。バッチ種別ごとに 1 つ。
/// @see DynamicBufferPool.hpp
DynamicVertexBufferPool s_linePool;
DynamicVertexBufferPool s_depthLinePool;
DynamicVertexBufferPool s_triPool;

/// @brief ResourceManager::Reset() の世代。変わったらシェーダー / CB / PSO を作り直す。
/// @note 頂点バッファのプールは DynamicVertexBufferPool::Acquire が自分で世代を見る。
uint32_t s_resetVersion = 0;

void EnsureInit(ResourceManager& resources)
{
    if (s_shader.IsValid() && s_resetVersion == resources.GetResetVersion())
        return;

    s_resetVersion = resources.GetResetVersion();
    s_shader   = resources.LoadShader("Assets/Shaders/Debug/DebugDraw.hlsl");
    s_cameraCB = resources.CreateConstantBuffer(sizeof(DebugCamCB));
    s_pso      = resources.CreatePipelineState({ RasterizerMode::SOLID,
                                         BlendMode::OPAQUE_BLEND,
                                         DepthMode::DEPTH_OFF });
    /// @note 深度は読むだけ。書くと後続の半透明パスを乱す。
    s_depthPso = resources.CreatePipelineState({ RasterizerMode::SOLID,
                                         BlendMode::OPAQUE_BLEND,
                                         DepthMode::DEPTH_READ });
    s_triPso   = resources.CreatePipelineState({ RasterizerMode::SOLID_NOCULL,
                                         BlendMode::ALPHA_BLEND,
                                         DepthMode::DEPTH_READ });

    assert(s_shader.IsValid() &&
           s_cameraCB.IsValid() && s_pso.IsValid() && s_depthPso.IsValid() && s_triPso.IsValid() &&
           "DebugDraw initialization failed");
}

void SubmitBatch(std::vector<DebugDrawVertex>& batch,
                 DynamicVertexBufferPool& pool,
                 ResourceHandle<PipelineStateTag> pipelineState,
                 PrimitiveTopology topology)
{
    if (batch.empty()) return;

    const ResourceHandle<BufferTag> vb =
        pool.Acquire(*s_resources, batch.size(), sizeof(DebugDrawVertex));
    if (vb.IsValid()) {
        s_resources->Update(vb, batch.data(), batch.size() * sizeof(DebugDrawVertex));

        DrawCall call;
        call.vertexBuffer       = vb;
        call.shader             = s_shader;
        call.pipelineState      = pipelineState;
        call.constantBuffers[0] = s_cameraCB;
        call.vertexCount        = static_cast<uint32_t>(batch.size());
        call.topology           = topology;

        s_renderer->Submit(call, *s_resources);
    }
    batch.clear();
}

[[nodiscard]] bool CanDraw()
{
    return s_renderer != nullptr || s_capture != nullptr;
}

/// @brief 記録中なら記録先へ、そうでなければバッチへ積む。満杯なら先に全バッチを Flush する。
void PushPrimitive(std::vector<DebugDrawVertex>& batch, const DebugDrawVertex* vertices, size_t count)
{
    const bool appended = AppendDebugPrimitive(batch, MAX_DEBUG_VERTICES, vertices, count,
                                               s_capture != nullptr, [] { DebugDraw::Flush(); });
    if (!appended)
        FBZZ_LOG_WARN("DebugDraw: dropped a primitive of %zu vertices (no renderer to flush)", count);
}

void AddSegmentTo(bool depthTested, const math::Vector3& a, const math::Vector3& b, const math::Vector4& color)
{
    const DebugDrawVertex vertices[2] = { { a, color }, { b, color } };
    if (s_capture)
        PushPrimitive(depthTested ? s_capture->depthLines : s_capture->overlayLines, vertices, 2);
    else
        PushPrimitive(depthTested ? s_depthBatch : s_batch, vertices, 2);
}

void AddSegment(const math::Vector3& a, const math::Vector3& b, const math::Vector4& color)
{
    AddSegmentTo(s_depthTest, a, b, color);
}

/// @brief axis1 / axis2 が張る平面に円弧を積む。
void AddArc(const math::Vector3& center,
            const math::Vector3& axis1,
            const math::Vector3& axis2,
            float radius, const math::Vector4& color,
            float fromAngle, float toAngle, int segments)
{
    const float step = (toAngle - fromAngle) / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        const float a0 = fromAngle + step * static_cast<float>(i);
        const float a1 = fromAngle + step * static_cast<float>(i + 1);
        const math::Vector3 p0 = center + axis1 * (radius * std::cos(a0)) + axis2 * (radius * std::sin(a0));
        const math::Vector3 p1 = center + axis1 * (radius * std::cos(a1)) + axis2 * (radius * std::sin(a1));
        AddSegment(p0, p1, color);
    }
}

void AddCircle(const math::Vector3& center,
               const math::Vector3& axis1,
               const math::Vector3& axis2,
               float radius, const math::Vector4& color)
{
    AddArc(center, axis1, axis2, radius, color, 0.0f, 2.0f * PI, CIRCLE_SEGMENTS);
}

void AddFilledTriangle(const math::Vector3& a, const math::Vector3& b, const math::Vector3& c,
                       const math::Vector4& color)
{
    const DebugDrawVertex vertices[3] = { { a, color }, { b, color }, { c, color } };
    PushPrimitive(s_capture ? s_capture->triangles : s_triBatch, vertices, 3);
}

/// @brief n に直交する正規直交 2 軸。n は正規化済みであること。
void BuildBasis(const math::Vector3& n, math::Vector3& outRight, math::Vector3& outUp)
{
    outRight = (std::abs(n.y) < 0.99f)
        ? math::Vector3::Cross(n, { 0, 1, 0 }).Normalized()
        : math::Vector3::Cross(n, { 1, 0, 0 }).Normalized();
    outUp = math::Vector3::Cross(outRight, n).Normalized();
}

void ReplayInto(std::vector<DebugDrawVertex>& batch, const std::vector<DebugDrawVertex>& source,
                size_t stride)
{
    for (size_t i = 0; i + stride <= source.size(); i += stride)
        PushPrimitive(batch, source.data() + i, stride);
}

} /// @note namespace

void DebugDraw::BeginFrame(IRenderer& r, ResourceManager& resources, const math::Matrix4& viewProjection)
{
    s_renderer  = &r;
    s_resources = &resources;
    EnsureInit(resources);

    DebugCamCB cb{ viewProjection };
    resources.Update(s_cameraCB, &cb, sizeof(cb));

    s_batch.clear();
    s_depthBatch.clear();
    s_triBatch.clear();
    s_depthTest = false;
}

void DebugDraw::Flush()
{
    if (!s_renderer) { s_batch.clear(); s_depthBatch.clear(); s_triBatch.clear(); return; }

    /// @note 深度ありを先に描く。後に描く深度なしの線 (ギズモ) が重なりで勝つ。
    SubmitBatch(s_depthBatch, s_depthLinePool, s_depthPso, PrimitiveTopology::LINE_LIST);
    SubmitBatch(s_batch,      s_linePool,      s_pso,      PrimitiveTopology::LINE_LIST);
    SubmitBatch(s_triBatch,   s_triPool,       s_triPso,   PrimitiveTopology::TRIANGLE_LIST);
}

void DebugDraw::BeginCapture(DebugDrawCapture& out)
{
    assert(!s_capture && "DebugDraw::BeginCapture does not nest");
    s_capture   = &out;
    s_depthTest = false;
}

void DebugDraw::EndCapture()
{
    s_capture   = nullptr;
    s_depthTest = false;
}

void DebugDraw::Replay(IRenderer& /*r*/, const DebugDrawCapture& capture, DebugDrawLayer layers)
{
    assert(s_renderer && !s_capture && "DebugDraw::Replay must be called between BeginFrame and Flush");
    const auto has = [layers](DebugDrawLayer layer) {
        return (static_cast<uint8_t>(layers) & static_cast<uint8_t>(layer)) != 0;
    };
    if (has(DebugDrawLayer::DepthTested)) {
        ReplayInto(s_depthBatch, capture.depthLines, 2);
        ReplayInto(s_triBatch,   capture.triangles,  3);
    }
    if (has(DebugDrawLayer::Overlay))
        ReplayInto(s_batch, capture.overlayLines, 2);
}

void DebugDraw::SetDepthTest(bool enabled) { s_depthTest = enabled; }

bool DebugDraw::IsDepthTest() { return s_depthTest; }

size_t DebugDraw::PendingLineVertices() { return s_batch.size(); }

size_t DebugDraw::PendingTriangleVertices() { return s_triBatch.size(); }

size_t DebugDraw::MaxBatchVertices() { return MAX_DEBUG_VERTICES; }

void DebugDraw::Line(IRenderer& /*r*/,
                     const math::Vector3& from, const math::Vector3& to,
                     const math::Vector4& color)
{
    assert(CanDraw() && "DebugDraw::BeginFrame must be called first");
    AddSegment(from, to, color);
}

void DebugDraw::LineDepthTested(IRenderer& /*r*/,
                                const math::Vector3& from, const math::Vector3& to,
                                const math::Vector4& color)
{
    assert(CanDraw() && "DebugDraw::BeginFrame must be called first");
    AddSegmentTo(true, from, to, color);
}

void DebugDraw::LineDashed(IRenderer& /*r*/,
                           const math::Vector3& from, const math::Vector3& to,
                           const math::Vector4& color,
                           float dashLength, int maxDashes)
{
    assert(CanDraw() && "DebugDraw::BeginFrame must be called first");

    const math::Vector3 delta = to - from;
    const float length = delta.Length();
    if (length <= math::EPSILON || dashLength <= 0.0f || maxDashes <= 1) {
        AddSegment(from, to, color);
        return;
    }

    const int period = std::clamp(
        static_cast<int>(std::lround(length / (dashLength * (1.0f / DASH_DUTY)))),
        1, maxDashes);
    const float step = 1.0f / static_cast<float>(period);
    for (int i = 0; i < period; ++i) {
        const float t0 = step * static_cast<float>(i);
        AddSegment(from + delta * t0, from + delta * (t0 + step * DASH_DUTY), color);
    }
}

size_t DebugDraw::DashedLineMaxVertices(int maxDashes)
{
    return static_cast<size_t>((std::max)(maxDashes, 1)) * 2u;
}

void DebugDraw::Polyline(IRenderer& /*r*/, std::span<const math::Vector3> points, bool closed,
                         const math::Vector4& color)
{
    assert(CanDraw() && "DebugDraw::BeginFrame must be called first");
    if (points.size() < 2) return;
    for (size_t i = 0; i + 1 < points.size(); ++i)
        AddSegment(points[i], points[i + 1], color);
    if (closed && points.size() > 2)
        AddSegment(points.back(), points.front(), color);
}

void DebugDraw::Box(IRenderer& r,
                    const math::Vector3& center, const math::Vector3& h,
                    const math::Vector4& color)
{
    Box(r, center, h, math::Quaternion::Identity(), color);
}

void DebugDraw::Box(IRenderer& /*r*/,
                    const math::Vector3& center, const math::Vector3& h,
                    const math::Quaternion& rotation,
                    const math::Vector4& color)
{
    assert(CanDraw() && "DebugDraw::BeginFrame must be called first");

    const math::Vector3 c[8] = {
        center + rotation * math::Vector3{-h.x, -h.y, -h.z},
        center + rotation * math::Vector3{+h.x, -h.y, -h.z},
        center + rotation * math::Vector3{+h.x, +h.y, -h.z},
        center + rotation * math::Vector3{-h.x, +h.y, -h.z},
        center + rotation * math::Vector3{-h.x, -h.y, +h.z},
        center + rotation * math::Vector3{+h.x, -h.y, +h.z},
        center + rotation * math::Vector3{+h.x, +h.y, +h.z},
        center + rotation * math::Vector3{-h.x, +h.y, +h.z},
    };

    AddSegment(c[0], c[1], color); AddSegment(c[1], c[2], color);
    AddSegment(c[2], c[3], color); AddSegment(c[3], c[0], color);
    AddSegment(c[4], c[5], color); AddSegment(c[5], c[6], color);
    AddSegment(c[6], c[7], color); AddSegment(c[7], c[4], color);
    AddSegment(c[0], c[4], color); AddSegment(c[1], c[5], color);
    AddSegment(c[2], c[6], color); AddSegment(c[3], c[7], color);
}

void DebugDraw::Sphere(IRenderer& /*r*/,
                       const math::Vector3& center, float radius,
                       const math::Vector4& color)
{
    assert(CanDraw() && "DebugDraw::BeginFrame must be called first");

    const math::Vector3 X{1,0,0}, Y{0,1,0}, Z{0,0,1};
    AddCircle(center, X, Y, radius, color);
    AddCircle(center, X, Z, radius, color);
    AddCircle(center, Y, Z, radius, color);
}

void DebugDraw::Capsule(IRenderer& r,
                        const math::Vector3& center, float radius, float halfHeight,
                        const math::Vector4& color)
{
    Capsule(r, center, radius, halfHeight, math::Quaternion::Identity(), color);
}

void DebugDraw::Capsule(IRenderer& /*r*/,
                        const math::Vector3& center, float radius, float halfHeight,
                        const math::Quaternion& rotation,
                        const math::Vector4& color)
{
    assert(CanDraw() && "DebugDraw::BeginFrame must be called first");

    const math::Vector3 X = rotation * math::Vector3{1,0,0};
    const math::Vector3 Y = rotation * math::Vector3{0,1,0};
    const math::Vector3 Z = rotation * math::Vector3{0,0,1};
    const math::Vector3 top    = center + Y * halfHeight;
    const math::Vector3 bottom = center - Y * halfHeight;
    const int half             = CIRCLE_SEGMENTS / 2;

    AddCircle(top,    X, Z, radius, color);
    AddCircle(bottom, X, Z, radius, color);

    AddArc(top,    X, Y, radius, color, 0.0f, PI,        half);
    AddArc(top,    Z, Y, radius, color, 0.0f, PI,        half);
    AddArc(bottom, X, Y, radius, color, PI,   2.0f * PI, half);
    AddArc(bottom, Z, Y, radius, color, PI,   2.0f * PI, half);

    AddSegment(top + X *  radius, bottom + X *  radius, color);
    AddSegment(top + X * -radius, bottom + X * -radius, color);
    AddSegment(top + Z *  radius, bottom + Z *  radius, color);
    AddSegment(top + Z * -radius, bottom + Z * -radius, color);
}

void DebugDraw::Circle(IRenderer& /*r*/, const math::Vector3& center, const math::Vector3& normal,
                       float radius, const math::Vector4& color)
{
    assert(CanDraw() && "DebugDraw::BeginFrame must be called first");
    const float length = normal.Length();
    if (length < 1e-6f || radius <= 0.0f) return;
    math::Vector3 right, up;
    BuildBasis(normal * (1.0f / length), right, up);
    AddCircle(center, right, up, radius, color);
}

void DebugDraw::Arc(IRenderer& /*r*/, const math::Vector3& center, const math::Vector3& normal,
                    const math::Vector3& fromDirection, float radius, float angleRadians,
                    const math::Vector4& color)
{
    assert(CanDraw() && "DebugDraw::BeginFrame must be called first");
    const float normalLength = normal.Length();
    if (normalLength < 1e-6f || radius <= 0.0f) return;
    const math::Vector3 n = normal * (1.0f / normalLength);
    const math::Vector3 planar = fromDirection - n * math::Vector3::Dot(fromDirection, n);
    const float planarLength = planar.Length();
    if (planarLength < 1e-6f) return;
    const math::Vector3 axis1 = planar * (1.0f / planarLength);
    const math::Vector3 axis2 = math::Vector3::Cross(n, axis1);
    const int segments = std::clamp(
        static_cast<int>(std::ceil(std::abs(angleRadians) / (2.0f * PI) * CIRCLE_SEGMENTS)),
        1, CIRCLE_SEGMENTS);
    AddArc(center, axis1, axis2, radius, color, 0.0f, angleRadians, segments);
}

void DebugDraw::Arrow(IRenderer& /*r*/,
                      const math::Vector3& from, const math::Vector3& to,
                      float headLength, float headRadius,
                      const math::Vector4& color)
{
    assert(CanDraw() && "DebugDraw::BeginFrame must be called first");

    const math::Vector3 diff = to - from;
    const float length = diff.Length();
    if (length < 1e-6f) return;

    const math::Vector3 n = diff * (1.0f / length);
    const float clampedHead = std::min(headLength, length);
    const math::Vector3 shaftTip = to - n * clampedHead;

    AddSegment(from, shaftTip, color);

    math::Vector3 right, up;
    BuildBasis(n, right, up);
    AddCircle(shaftTip, right, up, headRadius, color);
    AddSegment(to, shaftTip + right * headRadius, color);
    AddSegment(to, shaftTip - right * headRadius, color);
    AddSegment(to, shaftTip + up    * headRadius, color);
    AddSegment(to, shaftTip - up    * headRadius, color);
}

void DebugDraw::Cone(IRenderer& /*r*/,
                     const math::Vector3& apex, const math::Vector3& direction,
                     float height, float baseRadius,
                     const math::Vector4& color)
{
    assert(CanDraw() && "DebugDraw::BeginFrame must be called first");

    const float len = direction.Length();
    if (len < 1e-6f || height < 1e-6f) return;

    const math::Vector3 n = direction * (1.0f / len);
    const math::Vector3 baseCenter = apex + n * height;

    math::Vector3 right, up;
    BuildBasis(n, right, up);
    AddCircle(baseCenter, right, up, baseRadius, color);
    AddSegment(apex, baseCenter + right * baseRadius, color);
    AddSegment(apex, baseCenter - right * baseRadius, color);
    AddSegment(apex, baseCenter + up    * baseRadius, color);
    AddSegment(apex, baseCenter - up    * baseRadius, color);
}

void DebugDraw::FilledPolygon(IRenderer& /*r*/, const math::Vector3* verts, size_t count,
                              const math::Vector4& color)
{
    assert(CanDraw() && "DebugDraw::BeginFrame must be called first");
    if (count < 3) return;
    for (size_t i = 1; i + 1 < count; ++i)
        AddFilledTriangle(verts[0], verts[i], verts[i + 1], color);
}

} /// @note namespace fbzz::renderer
