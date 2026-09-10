/// @file    DebugDraw.cpp
/// @brief   ワイヤーフレームのデバッグ描画実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// フレーム内に積まれた線分をバッチ化し、LINE_LIST の DrawCall として送る。
/// 物理・Scene の可視化から呼ばれるが、状態は描画フレーム内に閉じる。
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/DynamicVertexBufferPool.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fbzz::renderer {

// =============================================================================
// ローカル型と定数
// =============================================================================

namespace {

struct DebugVertex {
    math::Vector3 position;
    math::Vector4 color;
};

struct DebugCamCB {
    math::Matrix4 viewProjection;
};

constexpr uint32_t MAX_DEBUG_VERTICES = 65536;
constexpr int      CIRCLE_SEGMENTS    = 24;
constexpr float    PI                 = 3.14159265358979f;
// 破線 1 周期のうち実線が占める割合。空きが狭すぎると下の線が見えず、
// 広すぎると破線側の形が読めなくなる。
constexpr float    DASH_DUTY          = 0.55f;

} // namespace

// =============================================================================
// 静的状態
// =============================================================================

static IRenderer* s_renderer = nullptr;
static ResourceManager* s_resources = nullptr;
static ResourceHandle<ShaderTag>        s_shader;
static ResourceHandle<ConstantBufferTag> s_cameraCB;
static ResourceHandle<PipelineStateTag> s_pso;
static ResourceHandle<PipelineStateTag> s_depthPso;
static ResourceHandle<PipelineStateTag> s_triPso;
static std::vector<DebugVertex>         s_batch;
static std::vector<DebugVertex>         s_depthBatch;
static std::vector<DebugVertex>         s_triBatch;

// Flush 1 回ぶんの頂点バッファを貸し出すプール。バッチ種別ごとに 1 つ持つ。
// Script の Gizmo (ScriptDebugDraw パス) とコライダー可視化 (DebugColliders パス) のように
// 1 フレームで 2 回以上 Flush する組み合わせが壊れないための仕組み
// (理由は DynamicVertexBufferPool.hpp を参照)。
static DynamicVertexBufferPool s_linePool;
static DynamicVertexBufferPool s_depthLinePool;
static DynamicVertexBufferPool s_triPool;

// =============================================================================
// ローカルヘルパー
// =============================================================================

// ResourceManager::Reset() の世代。シェーダー / CB / PSO は実体ごと捨てられるので、
// 世代が変わったら «IsValid だが失効しているハンドル» を握ったままになる。
// WHY 頂点バッファのプールを気にしなくてよいか: DynamicVertexBufferPool が自分で
//     世代を見てキャッシュを捨てる (DynamicVertexBufferPool::Acquire)。
static uint32_t s_resetVersion = 0;

static void EnsureInit(ResourceManager& resources)
{
    if (s_shader.IsValid() && s_resetVersion == resources.GetResetVersion())
        return; // 初期化済み

    s_resetVersion = resources.GetResetVersion();
    s_shader   = resources.LoadShader("Assets/Shaders/Debug/DebugDraw.hlsl");
    s_cameraCB = resources.CreateConstantBuffer(sizeof(DebugCamCB));
    s_pso      = resources.CreatePipelineState({ RasterizerMode::SOLID,
                                         BlendMode::OPAQUE_BLEND,
                                         DepthMode::DEPTH_OFF });
    // 深度テストあり (書き込みなし) の線分用。グリッドなど「世界に置かれた線」を
    // シーンジオメトリに遮蔽させる。深度書き込みをしないのは後続の半透明パスを乱さないため。
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

// 1 バッチを貸出バッファへ載せて Submit し、バッチを空にする。
static void SubmitBatch(std::vector<DebugVertex>& batch,
                        DynamicVertexBufferPool& pool,
                        ResourceHandle<PipelineStateTag> pipelineState,
                        PrimitiveTopology topology)
{
    if (batch.empty()) return;

    const ResourceHandle<BufferTag> vb =
        pool.Acquire(*s_resources, batch.size(), sizeof(DebugVertex));
    if (vb.IsValid()) {
        s_resources->Update(vb, batch.data(), batch.size() * sizeof(DebugVertex));

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

static void AddSegment(const math::Vector3& a, const math::Vector3& b, const math::Vector4& color)
{
    if (s_batch.size() + 2 > MAX_DEBUG_VERTICES)
    {
        FBZZ_LOG_WARN("DebugDraw: vertex batch reached max capacity (%u)", MAX_DEBUG_VERTICES);
        return;
    }
    s_batch.push_back({ a, color });
    s_batch.push_back({ b, color });
}

static void AddSegmentDepthTested(const math::Vector3& a, const math::Vector3& b, const math::Vector4& color)
{
    if (s_depthBatch.size() + 2 > MAX_DEBUG_VERTICES)
    {
        FBZZ_LOG_WARN("DebugDraw: depth-tested vertex batch reached max capacity (%u)", MAX_DEBUG_VERTICES);
        return;
    }
    s_depthBatch.push_back({ a, color });
    s_depthBatch.push_back({ b, color });
}

// axis1/axis2 が張る平面に円弧を追加
static void AddArc(const math::Vector3& center,
                   const math::Vector3& axis1,
                   const math::Vector3& axis2,
                   float radius, const math::Vector4& color,
                   float fromAngle, float toAngle, int segments)
{
    float step = (toAngle - fromAngle) / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i)
    {
        float a0 = fromAngle + step * i;
        float a1 = fromAngle + step * (i + 1);
        math::Vector3 p0 = center + axis1 * (radius * std::cos(a0))
                                  + axis2 * (radius * std::sin(a0));
        math::Vector3 p1 = center + axis1 * (radius * std::cos(a1))
                                  + axis2 * (radius * std::sin(a1));
        AddSegment(p0, p1, color);
    }
}

static void AddCircle(const math::Vector3& center,
                      const math::Vector3& axis1,
                      const math::Vector3& axis2,
                      float radius, const math::Vector4& color)
{
    AddArc(center, axis1, axis2, radius, color, 0.0f, 2.0f * PI, CIRCLE_SEGMENTS);
}

static void AddFilledTriangle(const math::Vector3& a, const math::Vector3& b, const math::Vector3& c,
                               const math::Vector4& color)
{
    if (s_triBatch.size() + 3 > MAX_DEBUG_VERTICES) {
        FBZZ_LOG_WARN("DebugDraw: tri batch reached max capacity (%u)", MAX_DEBUG_VERTICES);
        return;
    }
    s_triBatch.push_back({ a, color });
    s_triBatch.push_back({ b, color });
    s_triBatch.push_back({ c, color });
}

// =============================================================================
// 公開 API
// =============================================================================

void DebugDraw::BeginFrame(IRenderer& r, ResourceManager& resources, const math::Matrix4& viewProjection)
{
    s_renderer = &r;
    s_resources = &resources;
    EnsureInit(resources);

    DebugCamCB cb{ viewProjection };
    resources.Update(s_cameraCB, &cb, sizeof(cb));

    s_batch.clear();
    s_depthBatch.clear();
    s_triBatch.clear();
}

void DebugDraw::Flush()
{
    if (!s_renderer) { s_batch.clear(); s_depthBatch.clear(); s_triBatch.clear(); return; }

    // 深度テストありの線分を先に描く。
    // WHY: 深度なしの線 (ギズモ) を後に描くことで、グリッドとギズモが重なった場合に
    //      「メッシュ越しでも見える」ギズモの性質を優先する。
    SubmitBatch(s_depthBatch, s_depthLinePool, s_depthPso, PrimitiveTopology::LINE_LIST);
    SubmitBatch(s_batch,      s_linePool,      s_pso,      PrimitiveTopology::LINE_LIST);
    SubmitBatch(s_triBatch,   s_triPool,       s_triPso,   PrimitiveTopology::TRIANGLE_LIST);
}

size_t DebugDraw::PendingLineVertices() { return s_batch.size(); }

size_t DebugDraw::PendingTriangleVertices() { return s_triBatch.size(); }

size_t DebugDraw::MaxBatchVertices() { return MAX_DEBUG_VERTICES; }

void DebugDraw::Line(IRenderer& /*r*/,
                     const math::Vector3& from, const math::Vector3& to,
                     const math::Vector4& color)
{
    assert(s_renderer && "DebugDraw::BeginFrame must be called first");
    AddSegment(from, to, color);
}

void DebugDraw::LineDepthTested(IRenderer& /*r*/,
                                const math::Vector3& from, const math::Vector3& to,
                                const math::Vector4& color)
{
    assert(s_renderer && "DebugDraw::BeginFrame must be called first");
    AddSegmentDepthTested(from, to, color);
}

void DebugDraw::LineDashed(IRenderer& /*r*/,
                           const math::Vector3& from, const math::Vector3& to,
                           const math::Vector4& color,
                           float dashLength, int maxDashes)
{
    assert(s_renderer && "DebugDraw::BeginFrame must be called first");

    const math::Vector3 delta = to - from;
    const float length = delta.Length();
    if (length <= math::EPSILON || dashLength <= 0.0f || maxDashes <= 1) {
        AddSegment(from, to, color);
        return;
    }

    // 実線 + 空きで 1 周期。周期数はワールド長から決め、上限で頭打ちにする。
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

void DebugDraw::Box(IRenderer& /*r*/,
                    const math::Vector3& center, const math::Vector3& h,
                    const math::Vector4& color)
{
    assert(s_renderer && "DebugDraw::BeginFrame must be called first");

    // 8 頂点
    math::Vector3 c[8] = {
        center + math::Vector3{-h.x, -h.y, -h.z},
        center + math::Vector3{+h.x, -h.y, -h.z},
        center + math::Vector3{+h.x, +h.y, -h.z},
        center + math::Vector3{-h.x, +h.y, -h.z},
        center + math::Vector3{-h.x, -h.y, +h.z},
        center + math::Vector3{+h.x, -h.y, +h.z},
        center + math::Vector3{+h.x, +h.y, +h.z},
        center + math::Vector3{-h.x, +h.y, +h.z},
    };

    // 12 辺
    // 下面
    AddSegment(c[0], c[1], color); AddSegment(c[1], c[2], color);
    AddSegment(c[2], c[3], color); AddSegment(c[3], c[0], color);
    // 上面
    AddSegment(c[4], c[5], color); AddSegment(c[5], c[6], color);
    AddSegment(c[6], c[7], color); AddSegment(c[7], c[4], color);
    // 側面
    AddSegment(c[0], c[4], color); AddSegment(c[1], c[5], color);
    AddSegment(c[2], c[6], color); AddSegment(c[3], c[7], color);
}

void DebugDraw::Box(IRenderer& /*r*/,
                    const math::Vector3& center, const math::Vector3& h,
                    const math::Quaternion& rotation,
                    const math::Vector4& color)
{
    assert(s_renderer && "DebugDraw::BeginFrame must be called first");

    math::Vector3 c[8] = {
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
    assert(s_renderer && "DebugDraw::BeginFrame must be called first");

    const math::Vector3 X{1,0,0}, Y{0,1,0}, Z{0,0,1};
    AddCircle(center, X, Y, radius, color); // XY 平面
    AddCircle(center, X, Z, radius, color); // XZ 平面
    AddCircle(center, Y, Z, radius, color); // YZ 平面
}

void DebugDraw::Capsule(IRenderer& /*r*/,
                        const math::Vector3& center, float radius, float halfHeight,
                        const math::Vector4& color)
{
    assert(s_renderer && "DebugDraw::BeginFrame must be called first");

    const math::Vector3 X{1,0,0}, Y{0,1,0}, Z{0,0,1};
    math::Vector3 top    = center + math::Vector3{0, halfHeight, 0};
    math::Vector3 bottom = center - math::Vector3{0, halfHeight, 0};
    int half             = CIRCLE_SEGMENTS / 2;

    // 上下リング (XZ 平面)
    AddCircle(top,    X, Z, radius, color);
    AddCircle(bottom, X, Z, radius, color);

    // 上半球の円弧 (0..pi, +Y 側) を XY / ZY 平面で追加
    AddArc(top, X, Y, radius, color, 0.0f,  PI, half);
    AddArc(top, Z, Y, radius, color, 0.0f,  PI, half);

    // 下半球の円弧 (pi..2pi, -Y 側)
    AddArc(bottom, X, Y, radius, color, PI, 2.0f * PI, half);
    AddArc(bottom, Z, Y, radius, color, PI, 2.0f * PI, half);

    // 側面 4 本
    AddSegment(top + X *  radius, bottom + X *  radius, color);
    AddSegment(top + X * -radius, bottom + X * -radius, color);
    AddSegment(top + Z *  radius, bottom + Z *  radius, color);
    AddSegment(top + Z * -radius, bottom + Z * -radius, color);
}

void DebugDraw::Capsule(IRenderer& /*r*/,
                        const math::Vector3& center, float radius, float halfHeight,
                        const math::Quaternion& rotation,
                        const math::Vector4& color)
{
    assert(s_renderer && "DebugDraw::BeginFrame must be called first");

    const math::Vector3 X = rotation * math::Vector3{1,0,0};
    const math::Vector3 Y = rotation * math::Vector3{0,1,0};
    const math::Vector3 Z = rotation * math::Vector3{0,0,1};
    math::Vector3 top    = center + Y * halfHeight;
    math::Vector3 bottom = center - Y * halfHeight;
    int half             = CIRCLE_SEGMENTS / 2;

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

void DebugDraw::Arrow(IRenderer& /*r*/,
                      const math::Vector3& from, const math::Vector3& to,
                      float headLength, float headRadius,
                      const math::Vector4& color)
{
    assert(s_renderer && "DebugDraw::BeginFrame must be called first");

    const math::Vector3 diff = to - from;
    const float length = diff.Length();
    if (length < 1e-6f) return;

    const math::Vector3 n = diff.Normalized();

    // ヘッド長が全体を超えないようクランプする
    const float clampedHead = std::min(headLength, length);
    const math::Vector3 shaftTip = to - n * clampedHead;

    // シャフト
    AddSegment(from, shaftTip, color);

    // コーンヘッド部分を Cone ヘルパーで描く
    // WHY: Arrow の先端コーンは底面が shaftTip、頂点が to なので
    //      Cone の direction を n (from → to 方向) にして apex = to に合わせる。
    //      内部で AddCircle / AddSegment を呼ぶ実装と同等にインライン化する。

    // n に直交する 2 軸を求める
    math::Vector3 right = (std::abs(n.y) < 0.99f)
        ? math::Vector3::Cross(n, {0,1,0}).Normalized()
        : math::Vector3::Cross(n, {1,0,0}).Normalized();
    const math::Vector3 up = math::Vector3::Cross(right, n).Normalized();

    // 底面の円
    AddCircle(shaftTip, right, up, headRadius, color);

    // 頂点から底面の等間隔 4 点へ線 (90° ごと)
    AddSegment(to, shaftTip + right *  headRadius, color);
    AddSegment(to, shaftTip - right *  headRadius, color);
    AddSegment(to, shaftTip + up    *  headRadius, color);
    AddSegment(to, shaftTip - up    *  headRadius, color);
}

void DebugDraw::Cone(IRenderer& /*r*/,
                     const math::Vector3& apex, const math::Vector3& direction,
                     float height, float baseRadius,
                     const math::Vector4& color)
{
    assert(s_renderer && "DebugDraw::BeginFrame must be called first");

    const float len = direction.Length();
    if (len < 1e-6f || height < 1e-6f) return;

    const math::Vector3 n = direction * (1.0f / len);
    const math::Vector3 baseCenter = apex + n * height;

    // 底面の直交基底を求める
    math::Vector3 right = (std::abs(n.y) < 0.99f)
        ? math::Vector3::Cross(n, {0,1,0}).Normalized()
        : math::Vector3::Cross(n, {1,0,0}).Normalized();
    const math::Vector3 up = math::Vector3::Cross(right, n).Normalized();

    // 底面の円
    AddCircle(baseCenter, right, up, baseRadius, color);

    // 頂点から底面の等間隔 4 点へ稜線 (90° ごと)
    AddSegment(apex, baseCenter + right *  baseRadius, color);
    AddSegment(apex, baseCenter - right *  baseRadius, color);
    AddSegment(apex, baseCenter + up    *  baseRadius, color);
    AddSegment(apex, baseCenter - up    *  baseRadius, color);
}

void DebugDraw::FilledPolygon(IRenderer& /*r*/, const math::Vector3* verts, size_t count,
                              const math::Vector4& color)
{
    assert(s_renderer && "DebugDraw::BeginFrame must be called first");
    if (count < 3) return;
    for (size_t i = 1; i + 1 < count; ++i)
        AddFilledTriangle(verts[0], verts[i], verts[i + 1], color);
}

} // namespace fbzz::renderer
