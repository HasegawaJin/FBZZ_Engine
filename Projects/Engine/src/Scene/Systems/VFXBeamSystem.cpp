/// @file    VFXBeamSystem.cpp
/// @brief   端点の解決と、たるみ・横ゆれを載せた折れ線の生成。
/// @author  Hasegawa Jin
/// @date    2026-08-26
#include <Engine/Scene/Systems/VFXBeamSystem.hpp>

#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/VFXBeamComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/VFXSystem.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {
namespace {

// 端点 1 つを解決する。実体があればそのワールド位置、無ければ与えられた座標。
// outMissing は「実体を指しているのに見つからない」ときだけ立つ
// (最初から座標指定の端点は «消えた» ではない)。
[[nodiscard]] math::Vector3 ResolveEndpoint(Scene& scene, const EntityRef& ref,
                                            const math::Vector3& fallback,
                                            const math::Vector3& offset,
                                            bool& outMissing)
{
    if (!ref.IsValid()) return fallback + offset;
    if (GameObject* target = scene.GetGameObject(ref.id))
        return target->transform.worldPosition + offset;
    outMissing = true;
    return fallback + offset;
}

// 線に直交する 2 軸。横ゆれをどちら向きへ出すかを決める。
// WHY 上方向を固定で使わないか: 真上・真下へ伸びるビーム (落雷・吸い上げ) で
//     外積が退化し、揺れが消えるか軸が飛ぶ。
void BuildBasis(const math::Vector3& direction, math::Vector3& outRight, math::Vector3& outUp)
{
    const math::Vector3 reference = std::abs(math::Vector3::Dot(direction, math::Vector3::UP)) > 0.99f
        ? math::Vector3::FORWARD : math::Vector3::UP;
    outRight = math::Vector3::Cross(direction, reference).NormalizedOr(math::Vector3::RIGHT);
    outUp = math::Vector3::Cross(outRight, direction).NormalizedOr(math::Vector3::UP);
}

} // namespace

ComponentAccess VFXBeamSystem::GetAccess() const
{
    return ComponentAccess{}.Unrestricted();
}

OrderingHints VFXBeamSystem::GetOrder() const
{
    // 端点は «今フレームのワールド位置»。VFXSystem が生存窓で Trail を
    // 起こした後に経路を書かないと、開いた最初のフレームだけ前回の形が出る。
    return OrderingHints{}.After<VFXSystem>();
}

void VFXBeamSystem::Update(SystemContext& ctx)
{
    for (const EntityID id : ctx.scene.GetEntities<VFXBeamComponent>()) {
        GameObject* owner = ctx.scene.GetGameObject(id);
        VFXBeamComponent* beamPtr = ctx.scene.GetComponent<VFXBeamComponent>(id);
        if (owner == nullptr || beamPtr == nullptr) continue;
        if (!owner->activeInHierarchy()) continue;
        VFXBeamComponent& beam = *beamPtr;

        // 見た目は同じ GameObject の Trail が持つ。片方だけ置いても «何も出ない» で済み、
        // 描画側を書き換える必要がない。
        auto* trail = owner->GetComponent<TrailComponent>();
        if (trail == nullptr) continue;

        if (!beam.enabled) {
            trail->beamPoints.clear();
            continue;
        }

        bool missing = false;
        const math::Vector3 from =
            ResolveEndpoint(ctx.scene, beam.fromEntity, beam.fromPoint, beam.fromOffset, missing);
        const math::Vector3 to =
            ResolveEndpoint(ctx.scene, beam.toEntity, beam.toPoint, beam.toOffset, missing);

        if (missing && beam.disableWhenEndpointMissing) {
            trail->beamPoints.clear();
            trail->enabled = false;
            continue;
        }

        trail->beamMode = true;
        trail->beamWorldSpace = true;
        trail->enabled = true;

        const int segments = std::clamp(beam.segments, 1, 64);
        const math::Vector3 delta = to - from;
        const float length = delta.Length();
        if (length < 0.0001f) {
            trail->beamPoints.clear();
            continue;
        }
        const math::Vector3 direction = delta * (1.0f / length);

        math::Vector3 right{}, up{};
        BuildBasis(direction, right, up);

        beam.phase += Time::deltaTime * beam.jitterFrequency;

        trail->beamPoints.clear();
        trail->beamPoints.reserve(static_cast<std::size_t>(segments) + 1);
        for (int index = 0; index <= segments; ++index) {
            const float t = static_cast<float>(index) / static_cast<float>(segments);
            math::Vector3 point = from + delta * t;

            // たるみは両端で 0、中央で最大。sin では «張った紐» に見えないので
            // 放物線 (4t(1-t)) を使う。
            if (beam.sag != 0.0f)
                point.y -= beam.sag * 4.0f * t * (1.0f - t);

            // 横ゆれも両端を固定する。端が揺れると «繋がっていない» に見える。
            if (beam.jitter != 0.0f) {
                const float taper = 4.0f * t * (1.0f - t);
                const float wave = t * beam.jitterWaves * 6.2831853f;
                point += right * (std::sin(wave + beam.phase) * beam.jitter * taper);
                point += up * (std::cos(wave * 1.37f + beam.phase * 0.8f) * beam.jitter * taper);
            }
            trail->beamPoints.push_back(point);
        }
    }
}

} // namespace fbzz::scene
