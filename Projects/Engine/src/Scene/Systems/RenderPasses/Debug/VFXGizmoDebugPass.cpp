/// @file    VFXGizmoDebugPass.cpp
/// @brief   パーティクル力場の影響体積とエミッター発生形状をワイヤーで描画する IRenderPass 実装。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// WHY: 力場もエミッター形状も「粒子の動きからしか推測できない見えない体積」で、
/// VFX の調整で最も当て推量になりやすい部分だった。radius 3.2 と 4.0 の違いを
/// 吹き上がり方から逆算するのは現実的でなく、「炎が横に千切れる」「上がりきらない」の
/// 原因が力場の半径なのか強さなのか切り分けられない。形が見えれば一目で済む。
///
/// 表示は操作用 Preview / Scene View 限定にすること (AI capture では常に off)。
/// 評価用の静止画にギズモが写り込むと、視覚判断が表示設定に左右されて再現しなくなる。
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/ParticleForceField.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {

namespace {

// 力場の種別ごとに色を変える。同じ場所に複数の力場を重ねる構成 (炎の Updraft + Convection)
// では、色が同じだとどちらの半径を触っているのか分からなくなる。
math::Vector4 ForceFieldColor(ParticleForceFieldType type)
{
    switch (type) {
    case ParticleForceFieldType::Wind:       return { 0.35f, 0.85f, 1.00f, 1.0f }; // 水色
    case ParticleForceFieldType::Attract:    return { 0.45f, 1.00f, 0.55f, 1.0f }; // 緑
    case ParticleForceFieldType::Repulse:    return { 1.00f, 0.55f, 0.35f, 1.0f }; // 橙
    case ParticleForceFieldType::Vortex:     return { 0.80f, 0.55f, 1.00f, 1.0f }; // 紫
    case ParticleForceFieldType::Turbulence: return { 1.00f, 0.85f, 0.35f, 1.0f }; // 黄
    case ParticleForceFieldType::Drag:       return { 0.65f, 0.68f, 0.75f, 1.0f }; // 灰
    }
    return { 1.0f, 1.0f, 1.0f, 1.0f };
}

// 力場の「向き」を矢印で示す。Wind は風向、Vortex は回転軸。
// それ以外 (Attract/Repulse/Turbulence/Drag) は等方なので向きを描かない
// — 意味の無い矢印を出すと「この向きに効くのか」と誤解させる。
bool HasDirection(ParticleForceFieldType type)
{
    return type == ParticleForceFieldType::Wind || type == ParticleForceFieldType::Vortex;
}

void DrawForceFields(RenderPassContext& ctx)
{
    for (auto& object : ctx.scene.GameObjects()) {
        if (!object.activeInHierarchy()) continue;
        const auto* field = object.GetComponent<ParticleForceField>();
        if (field == nullptr || !field->enabled) continue;
        // radius <= 0 はシーン全体へ減衰なしに効く設定。描くべき境界が存在しないので、
        // 「無限」であることが伝わるよう中心に小さなマーカーだけ置く。
        const math::Vector3  center = object.transform.worldPosition;
        const math::Vector4  color  = ForceFieldColor(field->fieldType);
        if (field->radius > 0.0f)
            renderer::DebugDraw::Sphere(ctx.renderer, center, field->radius, color);
        else
            renderer::DebugDraw::Sphere(ctx.renderer, center, 0.25f, color);

        if (!HasDirection(field->fieldType)) continue;
        // direction はローカル指定なので、Transform の回転でワールドへ移す
        // (ParticleForceField.hpp の規約に合わせる — ここがずれると矢印だけ嘘になる)。
        math::Vector3 direction = object.transform.worldRotation * field->direction;
        const float length = direction.Length();
        if (length <= 1.0e-5f) continue;
        direction = direction * (1.0f / length);
        // 矢印の長さは影響半径に比例させる。固定長だと大きな力場で見えなくなる。
        const float arrowLength = field->radius > 0.0f
            ? (std::max)(field->radius * 0.6f, 0.3f) : 1.0f;
        renderer::DebugDraw::Arrow(ctx.renderer, center, center + direction * arrowLength,
                                   arrowLength * 0.18f, arrowLength * 0.06f, color);
    }
}

void DrawEmitterShapes(RenderPassContext& ctx)
{
    constexpr math::Vector4 kShapeColor    = { 0.40f, 0.95f, 0.80f, 1.0f };
    constexpr math::Vector4 kVelocityColor = { 1.00f, 0.72f, 0.30f, 1.0f };

    for (auto& object : ctx.scene.GameObjects()) {
        if (!object.activeInHierarchy()) continue;
        const auto* emitter = object.GetComponent<ParticleEmitter>();
        if (emitter == nullptr || !emitter->settings.enabled) continue;
        // 発生原点は Transform に emitPosition を足した位置。エミッターを親へぶら下げる
        // 構成 (VFX Graph のノード) では、この差分が見えないと「なぜここから出るのか」が掴めない。
        const math::Vector3 origin = object.transform.worldPosition
            + object.transform.worldRotation * emitter->settings.emitPosition;

        switch (emitter->settings.shape) {
        case ParticleEmitterShape::Sphere:
            renderer::DebugDraw::Sphere(ctx.renderer, origin,
                                        (std::max)(emitter->settings.sphereRadius, 0.001f), kShapeColor);
            break;
        case ParticleEmitterShape::Cone: {
            // SpawnParticle は +Y をコーン軸に取る。ギズモも同じ軸で描かないと、
            // 傾けたエミッターで見た目と実際の噴出方向がずれる。
            const math::Vector3 axis = object.transform.worldRotation * math::Vector3{ 0.0f, 1.0f, 0.0f };
            constexpr float kDeg2Rad = 3.14159265f / 180.0f;
            const float height = (std::max)(emitter->settings.coneRadius, 0.5f);
            const float baseRadius = emitter->settings.coneRadius
                + std::tan((std::max)(emitter->settings.coneAngleDegrees, 0.0f) * kDeg2Rad) * height;
            renderer::DebugDraw::Cone(ctx.renderer, origin, axis, height, baseRadius, kShapeColor);
            break;
        }
        case ParticleEmitterShape::Box:
            renderer::DebugDraw::Box(ctx.renderer, origin, emitter->settings.boxExtents,
                                     object.transform.worldRotation, kShapeColor);
            break;
        case ParticleEmitterShape::Point:
        case ParticleEmitterShape::MeshSurface:
        default:
            // Point は体積を持たない。MeshSurface は形状がメッシュそのもので、
            // それは MeshRenderer 側のギズモが担当するため、ここでは原点だけ示す。
            renderer::DebugDraw::Sphere(ctx.renderer, origin, 0.06f, kShapeColor);
            break;
        }

        // 初速の向きと大きさ。上昇力を emitVelocity と gravity と力場のどこで作っているのかが
        // 分からなくなるのが VFX で最も多い事故なので、初速だけは常に見えるようにする。
        const math::Vector3 velocity = object.transform.worldRotation * emitter->settings.emitVelocity;
        const float speed = velocity.Length();
        if (speed <= 1.0e-4f) continue;
        // 1 秒後の到達点を矢印の先端にする (「秒速いくつか」がそのまま長さで読める)。
        const math::Vector3 tip = origin + velocity;
        renderer::DebugDraw::Arrow(ctx.renderer, origin, tip,
                                   (std::min)(speed * 0.18f, 0.4f),
                                   (std::min)(speed * 0.06f, 0.12f), kVelocityColor);
    }
}

} // namespace

std::string_view VFXGizmoDebugPass::Name() const { return "VFXGizmoDebug"; }

std::vector<renderer::RenderGraph::ResourceAccess>
VFXGizmoDebugPass::DeclareAccesses(const RenderPassContext&) const
{
    using U = renderer::RenderGraph::ResourceUsage;
    return { { "HDR", U::ReadWrite } };
}

void VFXGizmoDebugPass::Execute(RenderPassContext& ctx)
{
    if (!ctx.settings.showVFXGizmos) return;

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    DrawForceFields(ctx);
    DrawEmitterShapes(ctx);
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
