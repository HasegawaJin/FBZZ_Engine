/// @file    FluidVolume.cpp
/// @brief   流体に浸かった剛体へ浮力・流れの抵抗を掛ける Volume。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#include <Physics/FluidVolume.hpp>
#include <Physics/FlowDrag.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cmath>
#include <utility>

namespace fbzz::physics
{
    std::shared_ptr<const BodyRadiusMap> MakeBodyRadii(const BodyVolumeMap& volumes)
    {
        auto radii = std::make_shared<BodyRadiusMap>();
        radii->reserve(volumes.size());
        for (const auto& [body, volume] : volumes) {
            const float radius = std::cbrt((std::max)(volume, 0.0f) * 3.0f / (4.0f * math::PI));
            (*radii)[body] = math::Clamp(radius, 0.05f, 50.0f);
        }
        return radii;
    }

    FluidVolume::FluidVolume(FluidVolumeDesc desc)
        : m_desc(std::move(desc))
        , m_time(m_desc.startTime)
    {
        if (m_desc.radii) {
            for (const auto& entry : *m_desc.radii)
                m_maxRadius = (std::max)(m_maxRadius, entry.second);
        }
    }

    bool FluidVolume::Contains(const math::Vector3& position) const
    {
        if (std::abs(position.x - m_desc.center.x) > m_desc.halfX ||
            std::abs(position.z - m_desc.center.z) > m_desc.halfZ)
            return false;

        /// @note 重心が表面より上でも、体の下側が浸かっていれば浮力は掛かる。
        ///       一番大きい体の半径ぶん上へ広げる。
        const float minY = m_desc.center.y - m_desc.surfaceHeightBound;
        const float maxY = m_desc.center.y + m_desc.surfaceHeightBound;
        if (position.y > maxY + m_maxRadius || position.y < minY - m_desc.depthLimit)
            return false;
        if (position.y <= minY + m_maxRadius && position.y >= maxY - m_desc.depthLimit)
            return true;
        const float surfaceY = SurfaceY(position.x, position.z);
        return position.y <= surfaceY + m_maxRadius
            && position.y >= surfaceY - m_desc.depthLimit;
    }

    void FluidVolume::Apply(RigidBody& body, float /*dt*/)
    {
        if (body.IsStatic()) return;

        const float radius = RadiusOf(body);
        const float mass = body.GetMass();
        const math::Vector3 center = body.GetPosition();
        const math::Quaternion rotation = body.GetRotation();

        /// @note 重心まわりの 4 点で表面と比べる。点ごとに沈み具合が違えば、その差が
        ///       «波の斜面に沿って傾く» トルクになる。重心 1 点だけだと、船が波の上で
        ///       水平のまま上下する。
        const float arm = radius * 0.6f;
        const math::Vector3 arms[4] = {
            rotation * math::Vector3{  arm, 0.0f, 0.0f },
            rotation * math::Vector3{ -arm, 0.0f, 0.0f },
            rotation * math::Vector3{ 0.0f, 0.0f,  arm },
            rotation * math::Vector3{ 0.0f, 0.0f, -arm },
        };
        float submersion = 0.0f;
        math::Vector3 torque = math::Vector3::ZERO;
        for (const math::Vector3& offset : arms) {
            const math::Vector3 probe = center + offset;
            const float minY = m_desc.center.y - m_desc.surfaceHeightBound;
            const float maxY = m_desc.center.y + m_desc.surfaceHeightBound;
            const float sub = probe.y + radius <= minY ? 1.0f
                : probe.y - radius >= maxY ? 0.0f
                : math::Clamp01((SurfaceY(probe.x, probe.z) - (probe.y - radius)) / (2.0f * radius));
            if (sub <= 0.0f) continue;
            submersion += sub * 0.25f;
            const math::Vector3 lift = math::Vector3::UP * (m_desc.buoyancy * mass * sub * 0.25f);
            /// @note NoWake で入れる理由: 浮力・抵抗は毎 substep 掛かる環境力。WakeUp すると、
            ///       範囲内で止まった body が永久に Sleep できず World::Step が重くなる。
            body.ApplyForceNoWake(lift);
            torque += math::Vector3::Cross(offset, lift);
        }
        if (submersion <= 0.0f) return;
        body.ApplyTorqueNoWake(torque);

        /// @note 抵抗は «流体に対する» 速度に掛ける。川では水流と同じ速さになるまで押し流される。
        /// @note 式は FlowVolume (風) と同じ FlowDragForce。違いは «沈み率で重み付けした
        ///       結合係数» を渡す点だけ。
        /// @note 係数をまだ水の側 (m_desc.drag) が持っているのは移行途中だから。体側へ寄せるのは
        ///       buoyancy.md 順序 3 以降 (密度ベースの法則へ置き換えるとき)。
        const math::Vector3 flow = m_desc.flowVelocity ? m_desc.flowVelocity(center)
                                                       : math::Vector3::ZERO;
        body.ApplyForceNoWake(FlowDragForce(m_desc.drag * submersion, mass,
                                            flow, body.GetVelocity()));
        /// @note 回転も流体が止める。止めないと、波で傾いた物体がいつまでも揺れ続ける。
        const float inertia = 0.4f * mass * radius * radius;
        body.ApplyTorqueNoWake(body.GetAngularVelocity() * (-m_desc.drag * inertia * submersion));
    }

    void FluidVolume::Tick(float dt)
    {
        m_time += dt;
    }

    float FluidVolume::SurfaceY(float worldX, float worldZ) const
    {
        if (!m_desc.surfaceHeight) return m_desc.center.y;
        return m_desc.center.y + m_desc.surfaceHeight(worldX, worldZ, m_time);
    }

    float FluidVolume::RadiusOf(const RigidBody& body) const
    {
        if (!m_desc.radii) return DEFAULT_BODY_RADIUS;
        const auto it = m_desc.radii->find(&body);
        return it != m_desc.radii->end() ? it->second : DEFAULT_BODY_RADIUS;
    }
} // namespace fbzz::physics
