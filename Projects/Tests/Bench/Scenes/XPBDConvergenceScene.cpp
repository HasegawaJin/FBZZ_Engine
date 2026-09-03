/// @file    XPBDConvergenceScene.cpp
/// @brief   substep 数を変えても静止位置が変わらないことを目で見る場面。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// XPBDSolverTests が «静止位置は substep 数に依らない» を数値で固定している。
/// ここではその «落ち着くまでの様子» を見る ── 数値が同じでも、刻みが粗いほうだけ
/// 一度深く沈んでから戻る、といった過渡は自動テストでは拾えない。
#include "Scenes.hpp"

#include <Physics/XPBDPlaneContact.hpp>
#include <Physics/XPBDSolver.hpp>

#include <imgui.h>

#include <array>
#include <cstdio>
#include <memory>

namespace fbzz::bench {

namespace {

constexpr float kStartHeight = 3.0f;
constexpr float kRadius      = 0.25f;
constexpr float kSpacing     = 1.6f;

/// 1 本の «落として止める» 試験台。substep 数だけが違う個体を並べる。
struct Column {
    physics::XPBDSolver                 solver;
    std::unique_ptr<physics::RigidBody> body = std::make_unique<physics::RigidBody>();
    int                                 substeps = 8;
    float                               deepest  = kStartHeight;

    void Build(int substepCount, float x)
    {
        substeps = substepCount;

        solver.ClearBodies();
        solver.ClearConstraints();

        body->SetMass(1.0f);
        body->SetPosition({x, kStartHeight, 0.0f});
        body->SetRotation(math::Quaternion::Identity());
        body->m_linearDrag  = 0.0f;
        body->m_angularDrag = 0.0f;

        solver.SetGravity({0.0f, -9.81f, 0.0f});
        solver.SetSubsteps(substeps);
        solver.AddBody(body.get());
        solver.AddConstraint(std::make_unique<physics::XPBDPlaneContact>(
            body.get(), math::Vector3::ZERO, kRadius, math::Vector3::UP, 0.0f));

        deepest = kStartHeight;
    }

    ~Column() { solver.ClearBodies(); }
};

class XPBDConvergenceScene final : public BenchScene {
public:
    XPBDConvergenceScene() { Reset(); }

    const char* Name() const override { return "XPBD: 刻みと静止位置"; }

    const char* WhatToLookFor() const override
    {
        return "3 つの球は substep 数だけが違う。落ち着いたあと、3 つとも同じ高さ "
               "(= 半径 0.25m) で止まっていれば正しい。高さが揃わない場合、compliance で "
               "拘束を書いた狙い (定常誤差が刻みに依らない) が壊れている。\n"
               "過渡の «最も沈んだ深さ» は刻みが粗いほど深くなってよい ── そこは差が出る。";
    }

    void Reset() override
    {
        m_elapsed = 0.0f;
        for (size_t i = 0; i < m_columns.size(); ++i) {
            m_columns[i].Build(kSubstepChoices[i],
                               (static_cast<float>(i) - 1.0f) * kSpacing);
        }
    }

    void Simulate(float dt) override
    {
        m_elapsed += dt;
        for (Column& column : m_columns) {
            column.solver.Step(dt);
            const float height = column.body->GetPosition().y;
            if (height < column.deepest) column.deepest = height;
        }
    }

    void DrawControls() override
    {
        ImGui::Text("経過 %.2f 秒", m_elapsed);
        ImGui::Separator();

        if (ImGui::BeginTable("##columns", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("substeps");
            ImGui::TableSetupColumn("高さ");
            ImGui::TableSetupColumn("最深");
            ImGui::TableSetupColumn("誤差");
            ImGui::TableHeadersRow();

            for (const Column& column : m_columns) {
                const float height = column.body->GetPosition().y;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d", column.substeps);
                ImGui::TableNextColumn(); ImGui::Text("%.5f", height);
                ImGui::TableNextColumn(); ImGui::Text("%.5f", column.deepest);
                ImGui::TableNextColumn(); ImGui::Text("%+.5f", height - kRadius);
            }
            ImGui::EndTable();
        }
    }

    void Draw(Viewport2D& view) override
    {
        view.DrawGrid(0.5f);
        view.DrawGroundLine(0.0f, colors::kGround);

        for (const Column& column : m_columns) {
            const math::Vector3 position = column.body->GetPosition();
            view.DrawCircle(position, kRadius, colors::kBody, true);
            // 静止すべき高さ。球の底がここに触れていれば正しい。
            view.DrawLine({position.x - 0.5f, kRadius, 0.0f}, {position.x + 0.5f, kRadius, 0.0f},
                          colors::kNormal, 1.0f);

            char label[64];
            std::snprintf(label, sizeof(label), "%d steps", column.substeps);
            view.DrawText({position.x - 0.4f, position.y + kRadius + 0.35f, 0.0f}, colors::kHint,
                          label);
        }
    }

    void ConfigureView(Viewport2D& view) override
    {
        view.SetPlane(ViewPlane::XY);
        view.SetFocus({0.0f, 1.4f, 0.0f}, 110.0f);
    }

private:
    static constexpr std::array<int, 3> kSubstepChoices{2, 8, 32};

    std::array<Column, 3> m_columns;
    float                 m_elapsed = 0.0f;
};

} // namespace

std::unique_ptr<BenchScene> MakeXPBDConvergenceScene()
{
    return std::make_unique<XPBDConvergenceScene>();
}

} // namespace fbzz::bench
