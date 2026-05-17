// FBZZ Engine
// DebugDraw.cpp | fbzz::renderer
// ワイヤーフレームのデバッグ描画ユーティリティ実装
#include <engine/Renderer/DebugDraw.hpp>
#include <engine/Renderer/DrawCall.hpp>
#include <engine/Renderer/ShaderManager.hpp>
#include <engine/Core/Logger.hpp>
#include <math/MathUtils.hpp>
#include <cassert>
#include <cmath>
#include <vector>

namespace fbzz::renderer {

// =============================================================================
// 内部型・定数
// =============================================================================

namespace {

struct DebugVertex {
    math::Vector3 position;
    math::Vector4 color;
};

struct DebugCamCB {
    math::Matrix4 viewProjection;
};

constexpr uint32_t MAX_DEBUG_VERTICES = 4096;
constexpr int      CIRCLE_SEGMENTS    = 24;
constexpr float    PI                 = 3.14159265358979f;

} // namespace

// =============================================================================
// 静的メンバ
// =============================================================================

static IRenderer*                       s_renderer  = nullptr;
static std::shared_ptr<IBuffer>         s_vb;
static std::shared_ptr<IShader>         s_shader;
static std::shared_ptr<IConstantBuffer> s_cameraCB;
static std::shared_ptr<IPipelineState>  s_pso;
static std::vector<DebugVertex>         s_batch;

// =============================================================================
// 内部ヘルパー
// =============================================================================

static void EnsureInit(IRenderer& r)
{
    if (s_vb) return; // 初期化済み

    s_vb       = r.CreateVertexBuffer(nullptr,
                                       MAX_DEBUG_VERTICES * sizeof(DebugVertex),
                                       sizeof(DebugVertex));
    s_shader   = ShaderManager::Load("assets/shaders/Debug.hlsl");
    s_cameraCB = r.CreateConstantBuffer(sizeof(DebugCamCB));
    s_pso      = r.CreatePipelineState({ RasterizerMode::SOLID,
                                         BlendMode::OPAQUE,
                                         DepthMode::DEPTH_OFF });

    assert(s_vb && s_shader && s_cameraCB && s_pso && "DebugDraw 初期化失敗");
}

static void AddSegment(const math::Vector3& a, const math::Vector3& b, const math::Vector4& color)
{
    if (s_batch.size() + 2 > MAX_DEBUG_VERTICES)
    {
        FBZZ_LOG_WARN("DebugDraw: 頂点バッファが満杯です (上限 %u)", MAX_DEBUG_VERTICES);
        return;
    }
    s_batch.push_back({ a, color });
    s_batch.push_back({ b, color });
}

// center + axis1*cos(t)*r + axis2*sin(t)*r の円弧 [fromAngle, toAngle) を追加する
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

// =============================================================================
// 公開 API
// =============================================================================

void DebugDraw::BeginFrame(IRenderer& r, const math::Matrix4& viewProjection)
{
    s_renderer = &r;
    EnsureInit(r);

    DebugCamCB cb{ viewProjection };
    s_cameraCB->Update(&cb, sizeof(cb));

    s_batch.clear();
}

void DebugDraw::Flush()
{
    if (!s_renderer || s_batch.empty()) { s_batch.clear(); return; }

    s_vb->Update(s_batch.data(), s_batch.size() * sizeof(DebugVertex));

    DrawCall call;
    call.vertexBuffer       = s_vb;
    call.shader             = s_shader;
    call.pipelineState      = s_pso;
    call.constantBuffers[0] = s_cameraCB;
    call.vertexCount        = static_cast<uint32_t>(s_batch.size());
    call.topology           = PrimitiveTopology::LINE_LIST;

    s_renderer->Submit(call);
    s_batch.clear();
}

void DebugDraw::Line(IRenderer& /*r*/,
                     const math::Vector3& from, const math::Vector3& to,
                     const math::Vector4& color)
{
    assert(s_renderer && "DebugDraw::BeginFrame を先に呼ぶこと");
    AddSegment(from, to, color);
}

void DebugDraw::Box(IRenderer& /*r*/,
                    const math::Vector3& center, const math::Vector3& h,
                    const math::Vector4& color)
{
    assert(s_renderer && "DebugDraw::BeginFrame を先に呼ぶこと");

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
    // 柱
    AddSegment(c[0], c[4], color); AddSegment(c[1], c[5], color);
    AddSegment(c[2], c[6], color); AddSegment(c[3], c[7], color);
}

void DebugDraw::Sphere(IRenderer& /*r*/,
                       const math::Vector3& center, float radius,
                       const math::Vector4& color)
{
    assert(s_renderer && "DebugDraw::BeginFrame を先に呼ぶこと");

    const math::Vector3 X{1,0,0}, Y{0,1,0}, Z{0,0,1};
    AddCircle(center, X, Y, radius, color); // XY 面
    AddCircle(center, X, Z, radius, color); // XZ 面
    AddCircle(center, Y, Z, radius, color); // YZ 面
}

void DebugDraw::Capsule(IRenderer& /*r*/,
                        const math::Vector3& center, float radius, float halfHeight,
                        const math::Vector4& color)
{
    assert(s_renderer && "DebugDraw::BeginFrame を先に呼ぶこと");

    const math::Vector3 X{1,0,0}, Y{0,1,0}, Z{0,0,1};
    math::Vector3 top    = center + math::Vector3{0, halfHeight, 0};
    math::Vector3 bottom = center - math::Vector3{0, halfHeight, 0};
    int half             = CIRCLE_SEGMENTS / 2;

    // 上下端の円リング (XZ 面)
    AddCircle(top,    X, Z, radius, color);
    AddCircle(bottom, X, Z, radius, color);

    // 上半球 (0 → π = Y 正側) を XY / ZY 面で描く
    AddArc(top, X, Y, radius, color, 0.0f,  PI, half);
    AddArc(top, Z, Y, radius, color, 0.0f,  PI, half);

    // 下半球 (π → 2π = Y 負側)
    AddArc(bottom, X, Y, radius, color, PI, 2.0f * PI, half);
    AddArc(bottom, Z, Y, radius, color, PI, 2.0f * PI, half);

    // 4 本の縦連絡線
    AddSegment(top + X *  radius, bottom + X *  radius, color);
    AddSegment(top + X * -radius, bottom + X * -radius, color);
    AddSegment(top + Z *  radius, bottom + Z *  radius, color);
    AddSegment(top + Z * -radius, bottom + Z * -radius, color);
}

} // namespace fbzz::renderer
