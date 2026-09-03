/// @file    XPBDJointChainScene.cpp
/// @brief   関節でつないだ鎖の垂れ方・可動域・ドライブの効きを目で見る場面。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// XPBDJointTests は «釣り合いの角度» を数値で固定している。ここで見たいのは
/// そこへ至る過程 ── ドライブが力負けして back-drive する様子や、可動域で
/// 止まったあとに震えていないか。どちらも静止状態の数値には出ない。
#include "Scenes.hpp"

#include <Physics/XPBDJoint.hpp>
#include <Physics/XPBDSolver.hpp>

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace fbzz::bench {

namespace {

constexpr float kBoneSpan = 0.7f;

class XPBDJointChainScene final : public BenchScene {
public:
    XPBDJointChainScene() { Reset(); }
    ~XPBDJointChainScene() override { Teardown(); }

    const char* Name() const override { return "XPBD: 関節の鎖"; }

    const char* WhatToLookFor() const override
    {
        return "根元 (左端) は固定。重力で鎖が垂れる。\n"
               "・ソケットが伸びていないこと (骨の長さが一定に見えること)\n"
               "・可動域を有効にすると、その角度で «止まる» こと。止まり際で震えるなら "
               "compliance か substep が足りない\n"
               "・ドライブのトルク上限を下げていくと、支えきれずに垂れ始めること "
               "(力負けした関節はオレンジで表示)\n"
               "・substep を減らすと崩れ、増やすと支えられるようになること";
    }

    void Reset() override
    {
        Teardown();

        m_solver.SetGravity({0.0f, m_gravity, 0.0f});
        m_solver.SetSubsteps(m_substeps);

        m_root = std::make_unique<physics::RigidBody>();
        m_root->SetMass(1.0f);
        m_root->m_isStatic = true;
        m_root->SetPosition(math::Vector3::ZERO);
        m_solver.AddBody(m_root.get());

        physics::RigidBody* previous = m_root.get();
        for (int i = 0; i < m_boneCount; ++i) {
            auto body = std::make_unique<physics::RigidBody>();
            body->SetMass(1.0f);
            body->SetPosition({kBoneSpan * static_cast<float>(i + 1), 0.0f, 0.0f});
            body->m_linearDrag  = m_damping;
            body->m_angularDrag = m_damping;
            m_solver.AddBody(body.get());

            auto joint = std::make_unique<physics::XPBDJoint>(previous, body.get());
            joint->Build({kBoneSpan * static_cast<float>(i), 0.0f, 0.0f},
                         math::Quaternion::Identity());
            ApplyJointSettings(*joint);
            m_joints.push_back(joint.get());
            m_solver.AddConstraint(std::move(joint));

            previous = body.get();
            m_bones.push_back(std::move(body));
        }

        m_elapsed = 0.0f;
    }

    void Simulate(float dt) override
    {
        m_elapsed += dt;
        m_solver.Step(dt);
    }

    void DrawControls() override
    {
        bool rebuild = false;
        rebuild |= ImGui::SliderInt("骨の数", &m_boneCount, 1, 10);
        rebuild |= ImGui::SliderFloat("減衰", &m_damping, 0.0f, 8.0f, "%.2f");
        rebuild |= ImGui::SliderFloat("重力", &m_gravity, -30.0f, 0.0f, "%.2f");

        if (ImGui::SliderInt("substeps", &m_substeps, 1, 64)) m_solver.SetSubsteps(m_substeps);

        ImGui::SeparatorText("可動域");
        bool jointDirty = false;
        jointDirty |= ImGui::Checkbox("有効##limits", &m_limitsEnabled);
        jointDirty |= ImGui::SliderFloat("振れ幅 [度]", &m_swingLimitDeg, 0.0f, 180.0f, "%.0f");

        ImGui::SeparatorText("ドライブ");
        jointDirty |= ImGui::Checkbox("有効##drive", &m_driveEnabled);
        jointDirty |= ImGui::SliderFloat("compliance", &m_driveCompliance, 0.0f, 1.0e-2f, "%.5f");
        jointDirty |= ImGui::SliderFloat("トルク上限 [N·m]", &m_maxTorque, 0.0f, 200.0f, "%.1f");

        if (jointDirty) {
            for (physics::XPBDJoint* joint : m_joints) ApplyJointSettings(*joint);
        }
        if (rebuild) Reset();

        ImGui::Separator();
        ImGui::Text("経過 %.2f 秒 / 関節 %zu 本", m_elapsed, m_joints.size());
        if (!m_bones.empty()) {
            const math::Vector3 tip = m_bones.back()->GetPosition();
            ImGui::Text("先端 (%+.3f, %+.3f)", tip.x, tip.y);
        }
    }

    void Draw(Viewport2D& view) override
    {
        view.DrawGrid(0.5f);

        math::Vector3 previous = m_root ? m_root->GetPosition() : math::Vector3::ZERO;
        view.DrawPoint(previous, colors::kGround, 6.0f);

        for (size_t i = 0; i < m_bones.size(); ++i) {
            const math::Vector3 position = m_bones[i]->GetPosition();
            const bool saturated = (i < m_joints.size()) && m_joints[i]->IsDriveSaturated();

            view.DrawLine(previous, position, saturated ? colors::kBodyAlt : colors::kBody, 3.0f);
            view.DrawCircle(position, 0.09f, saturated ? colors::kBodyAlt : colors::kBody, true);
            previous = position;
        }

        if (!m_joints.empty()) {
            char label[96];
            std::snprintf(label, sizeof(label), "根元のたわみ %.1f 度",
                          m_joints.front()->GetSwingAngleZ() * 57.2957795f);
            view.DrawText({0.1f, 0.35f, 0.0f}, colors::kHint, label);
        }
    }

    void ConfigureView(Viewport2D& view) override
    {
        view.SetPlane(ViewPlane::XY);
        view.SetFocus({2.0f, -1.5f, 0.0f}, 110.0f);
    }

private:
    void ApplyJointSettings(physics::XPBDJoint& joint) const
    {
        const float swing = m_swingLimitDeg * 0.01745329252f;

        physics::XPBDJointLimits& limits = joint.Limits();
        limits.enabled   = m_limitsEnabled;
        limits.twistMin  = -swing;
        limits.twistMax  = swing;
        limits.swingMinY = -swing;
        limits.swingMaxY = swing;
        limits.swingMinZ = -swing;
        limits.swingMaxZ = swing;

        physics::XPBDJointDrive& drive = joint.Drive();
        drive.enabled    = m_driveEnabled;
        drive.compliance = m_driveCompliance;
        drive.damping    = 1.0f;
        drive.maxTorque  = m_maxTorque;
        drive.target     = math::Quaternion::Identity();
    }

    /// WHY 明示的に順序を決めるか: 拘束は剛体を生ポインタで持つ。剛体を先に壊すと
    ///     ソルバが破棄済みの剛体を触る。必ず拘束 → ボディ登録 → 実体、の順で外す。
    void Teardown()
    {
        m_solver.ClearConstraints();
        m_solver.ClearBodies();
        m_joints.clear();
        m_bones.clear();
        m_root.reset();
    }

    physics::XPBDSolver                              m_solver;
    std::unique_ptr<physics::RigidBody>              m_root;
    std::vector<std::unique_ptr<physics::RigidBody>> m_bones;
    std::vector<physics::XPBDJoint*>                 m_joints;

    int   m_boneCount = 4;
    int   m_substeps  = 8;
    float m_damping   = 0.5f;
    float m_gravity   = -9.81f;
    float m_elapsed   = 0.0f;

    bool  m_limitsEnabled  = false;
    float m_swingLimitDeg  = 45.0f;
    bool  m_driveEnabled   = false;
    float m_driveCompliance = 1.0e-4f;
    float m_maxTorque      = 0.0f;
};

} // namespace

std::unique_ptr<BenchScene> MakeXPBDJointChainScene()
{
    return std::make_unique<XPBDJointChainScene>();
}

} // namespace fbzz::bench
