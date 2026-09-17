/// @file    VFXLineSystem.cpp
/// @brief   VFX Line の端点の解決・形の生成・帯メッシュと per-instance パラメーターの書き込み
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include <Engine/Scene/Systems/VFXLineSystem.hpp>

#include <Math/CurlNoise.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/Components/VFXLineComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/VFXSystem.hpp>
#include <Engine/Scene/VFXLineGeometry.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fbzz::scene {
namespace {

math::Vector3 DivideSafe(const math::Vector3& value, const math::Vector3& scale)
{
    const auto divide = [](float a, float b) { return std::fabs(b) > 1.0e-6f ? a / b : 0.0f; };
    return { divide(value.x, scale.x), divide(value.y, scale.y), divide(value.z, scale.z) };
}

/// 線を出さないフレームは帯を畳む。前の形が残って «消したのに光っている» にならないように。
void Hide(GameObject& go)
{
    if (auto* procedural = go.GetComponent<ProceduralMeshComponent>()) procedural->enabled = false;
}

} // namespace

ComponentAccess VFXLineSystem::GetAccess() const
{
    return ComponentAccess{}.Unrestricted();
}

OrderingHints VFXLineSystem::GetOrder() const
{
    /// @note 端点は «今フレームのワールド位置»。VFXSystem が生存窓で有効 / 無効を決めた後に形を作る。
    return OrderingHints{}.After<VFXSystem>();
}

void VFXLineSystem::Update(SystemContext& ctx)
{
    const float dt = (std::max)(Time::deltaTime, 0.0f);
    std::vector<VFXLineStrand> strands;
    for (const EntityID id : ctx.scene.GetEntities<VFXLineComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        VFXLineComponent* linePtr = ctx.scene.GetComponent<VFXLineComponent>(id);
        if (go == nullptr || linePtr == nullptr) continue;
        VFXLineComponent& line = *linePtr;
        if (!go->activeInHierarchy() || !line.enabled) {
            Hide(*go);
            continue;
        }

        /// @note 端点。実体を指していて見つからないときだけ «消えた» とみなす (最初から座標指定は消えない)。
        const Transform& tf = go->transform;
        bool missing = false;
        math::Vector3 from = tf.worldPosition + line.fromOffset;
        if (line.fromEntity.IsValid()) {
            if (GameObject* target = ctx.scene.GetGameObject(line.fromEntity.id))
                from = target->transform.worldPosition + line.fromOffset;
            else
                missing = true;
        }
        math::Vector3 to = tf.worldPosition
            + tf.worldRotation * math::Vector3{ line.toPoint.x * tf.worldScale.x, line.toPoint.y * tf.worldScale.y,
                                                line.toPoint.z * tf.worldScale.z };
        if (line.toEntity.IsValid()) {
            if (GameObject* target = ctx.scene.GetGameObject(line.toEntity.id))
                to = target->transform.worldPosition;
            else
                missing = true;
        }
        if (missing && line.disableWhenEndpointMissing) {
            Hide(*go);
            continue;
        }

        line.time += dt;
        line.strikeClock += dt;
        if (line.mode == VFXLineMode::Lightning && line.strikeRate > 0.0f) {
            const float interval = 1.0f / line.strikeRate;
            if (line.strikeClock >= interval) {
                line.strikeClock = std::fmod(line.strikeClock, interval);
                ++line.strikeIndex;
            }
        }
        if (line.mode == VFXLineMode::Lightning) {
            const std::uint32_t strikeSeed =
                math::PcgHash(static_cast<std::uint32_t>(line.seed) * 9781u + line.strikeIndex * 6271u);
            GenerateLightning(line, from, to, strikeSeed, line.time, strands);
        } else {
            GenerateBeam(line, from, to, line.time, strands);
        }

        auto* procedural = go->GetComponent<ProceduralMeshComponent>();
        if (procedural == nullptr) procedural = &go->AddComponent<ProceduralMeshComponent>();
        procedural->enabled = true;
        procedural->materialPath = line.materialPath;

        /// @note 中心線だけを積む。帯へ広げるのは VS (VFXLine.hlsl) で、描いているビューのカメラへ向ける。
        ///       NORMAL = 線の向き / TANGENT.x = 半幅 / UV = (進み, 左右) / COLOR = 本流と枝の明るさ
        MeshBuilder& builder = procedural->builder;
        builder.Clear();
        const math::Quaternion inverseRotation = tf.worldRotation.Inverse();
        const float halfWidth = (std::max)(line.width, 0.0f) * 0.5f;
        for (const VFXLineStrand& strand : strands) {
            const std::size_t count = strand.points.size();
            if (count < 2) continue;
            const math::Vector4 tint{ strand.brightness, strand.brightness, strand.brightness, 1.0f };
            std::uint32_t previous = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(count - 1);
                const math::Vector3& ahead = strand.points[(std::min)(i + 1, count - 1)];
                const math::Vector3& behind = strand.points[i > 0 ? i - 1 : 0];
                MeshVertex left{};
                left.position = DivideSafe(inverseRotation * (strand.points[i] - tf.worldPosition), tf.worldScale);
                left.normal = DivideSafe(inverseRotation * (ahead - behind), tf.worldScale);
                left.tangent = { halfWidth * strand.width * (strand.startTaper + (strand.endTaper - strand.startTaper) * t),
                                 0.0f, 0.0f };
                left.uv = { t, 0.0f };
                left.color = tint;
                MeshVertex right = left;
                right.uv = { t, 1.0f };
                const std::uint32_t index = builder.AddVertex(left);
                builder.AddVertex(right);
                if (i > 0) {
                    builder.AddTriangle(previous, previous + 1, index);
                    builder.AddTriangle(previous + 1, index + 1, index);
                }
                previous = index;
            }
        }
        procedural->dirty = MeshDirty::All;

        /// @note 色と時刻は per-instance の上書きで渡す (同じ .mat を共有する線どうしが汚し合わない)。
        auto* material = go->GetComponent<MaterialComponent>();
        if (material == nullptr) material = &go->AddComponent<MaterialComponent>();
        const float brightness = VFXLineBrightness(line, line.time, line.strikeIndex, line.strikeClock);
        material->paramOverrides["albedo"] = { line.color.x, line.color.y, line.color.z, line.color.w };
        material->paramOverrides["coreColor"] = { line.coreColor.x, line.coreColor.y, line.coreColor.z, line.coreColor.w };
        material->paramOverrides["intensity"] = { (std::max)(line.intensity, 0.0f) * brightness };
        material->paramOverrides["phase"] = { line.time };
        material->paramOverrides["coreWidth"] = { line.coreWidth };
        material->paramOverrides["breakup"] = { line.breakup };
        material->paramOverrides["pulseSpeed"] = { line.pulseSpeed };
        material->paramOverrides["pulseDensity"] = { line.pulseDensity };
    }
}

} // namespace fbzz::scene
