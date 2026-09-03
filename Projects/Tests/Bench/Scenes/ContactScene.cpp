/// @file    ContactScene.cpp
/// @brief   GJK / EPA が返す衝突法線と貫通深度を、形状を動かしながら目で見る場面。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// GJKTests / EPATests が «特定の配置での答え» を固定しているのに対し、ここでは
/// 連続的に動かしたときに法線が飛ばないかを見る。法線が突然反転する配置は、
/// 実際のゲームでは «壁際で弾かれる» «床を貫通する» として出る。
#include "Scenes.hpp"

#include <Physics/EPA.hpp>
#include <Physics/GJK.hpp>

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <memory>

namespace fbzz::bench {

namespace {

struct Sphere {
    math::Vector3 center;
    float         radius = 1.0f;

    static math::Vector3 Support(const void* shape, const math::Vector3& dir)
    {
        const auto& self = *static_cast<const Sphere*>(shape);
        return self.center + dir.NormalizedOr(math::Vector3::RIGHT) * self.radius;
    }
};

struct Box {
    math::Vector3 center;
    math::Vector3 halfExtents{1.0f, 1.0f, 1.0f};

    static math::Vector3 Support(const void* shape, const math::Vector3& dir)
    {
        const auto& self = *static_cast<const Box*>(shape);
        return {self.center.x + (dir.x >= 0.0f ? self.halfExtents.x : -self.halfExtents.x),
                self.center.y + (dir.y >= 0.0f ? self.halfExtents.y : -self.halfExtents.y),
                self.center.z + (dir.z >= 0.0f ? self.halfExtents.z : -self.halfExtents.z)};
    }
};

enum class ShapeKind { Sphere, Box };

class ContactScene final : public BenchScene {
public:
    ContactScene() { Reset(); }

    const char* Name() const override { return "GJK / EPA: 接触の法線と深さ"; }

    const char* WhatToLookFor() const override
    {
        return "B をスライダーで動かす。緑の矢印が A から B へ «押し出す向き» を指し、"
               "赤い線分が貫通量を表す。\n"
               "・重なりを浅くしていくと、深さが 0 に向かって連続的に減ること\n"
               "・B をゆっくり回り込ませたとき、法線が滑らかに向きを変えること "
               "(箱どうしでは面の切り替わりで飛ぶのが正常)\n"
               "・離した瞬間に «接触なし» へ切り替わり、黄色い線 (GJK_Distance の最近傍点) "
               "が両形状の «一番近いところ» どうしを結ぶこと\n"
               "・深さ 0 と隙間 0 が同じ位置で入れ替わること";
    }

    void Reset() override
    {
        m_a = Sphere{math::Vector3::ZERO, 1.0f};
        m_b = Sphere{math::Vector3(1.4f, 0.4f, 0.0f), 1.0f};
        m_boxA = Box{math::Vector3::ZERO, math::Vector3(1.0f, 1.0f, 1.0f)};
        m_boxB = Box{math::Vector3(1.4f, 0.4f, 0.0f), math::Vector3(1.0f, 1.0f, 1.0f)};
        m_kindA = ShapeKind::Sphere;
        m_kindB = ShapeKind::Sphere;
        m_orbit = false;
        m_angle = 0.0f;
        Evaluate();
    }

    void Simulate(float dt) override
    {
        if (m_orbit) {
            m_angle += dt * 0.6f;
            const float radius = 1.4f;
            SetCenterB({std::cos(m_angle) * radius, std::sin(m_angle) * radius, 0.0f});
        }
        Evaluate();
    }

    void DrawControls() override
    {
        bool dirty = false;

        dirty |= ShapeCombo("A の形", m_kindA);
        dirty |= ShapeCombo("B の形", m_kindB);
        ImGui::Separator();

        math::Vector3 center = CenterB();
        float         values[2]{center.x, center.y};
        if (ImGui::SliderFloat2("B の位置", values, -4.0f, 4.0f, "%.3f")) {
            SetCenterB({values[0], values[1], 0.0f});
            dirty = true;
        }
        ImGui::Checkbox("A のまわりを回す", &m_orbit);

        ImGui::Separator();
        if (m_gjk.intersects) {
            ImGui::TextColored({0.48f, 0.90f, 0.55f, 1.0f}, "交差あり");
            ImGui::Text("単体の頂点数: %d  (EPA は 4 を要求する)", m_gjk.simplex.size);
            if (m_epa.valid) {
                ImGui::Text("法線 (%+.3f, %+.3f, %+.3f)", m_epa.normal.x, m_epa.normal.y,
                            m_epa.normal.z);
                ImGui::Text("深さ %.4f m", m_epa.depth);
            } else {
                ImGui::TextColored({1.0f, 0.36f, 0.41f, 1.0f},
                                   "EPA が接触を作れなかった (単体が四面体でない)");
            }
        } else {
            ImGui::TextColored({0.74f, 0.77f, 0.81f, 1.0f}, "交差なし");
            ImGui::Text("隙間 %.4f m", m_gap.distance);
        }

        if (dirty) Evaluate();
    }

    void Draw(Viewport2D& view) override
    {
        view.DrawGrid(0.5f);

        DrawShape(view, m_kindA, m_a, m_boxA, colors::kBody);
        DrawShape(view, m_kindB, m_b, m_boxB, colors::kBodyAlt);

        if (!m_gjk.intersects) {
            // 離れているときは最近傍点どうしを結ぶ。長さがそのまま隙間。
            view.DrawLine(m_gap.closestA, m_gap.closestB, colors::kQuery, 2.0f);
            view.DrawPoint(m_gap.closestA, colors::kQuery, 5.0f);
            view.DrawPoint(m_gap.closestB, colors::kQuery, 5.0f);

            char gapLabel[64];
            std::snprintf(gapLabel, sizeof(gapLabel), "gap %.3f", m_gap.distance);
            view.DrawText((m_gap.closestA + m_gap.closestB) * 0.5f +
                              math::Vector3(0.0f, 0.15f, 0.0f),
                          colors::kQuery, gapLabel);
            return;
        }
        if (!m_epa.valid) return;

        // 押し出す向きを A の接触点から伸ばす。長さは «見える» 固定倍率。
        view.DrawArrow(m_epa.contactA, m_epa.contactA + m_epa.normal * 1.0f, colors::kNormal, 2.5f);
        view.DrawLine(m_epa.contactA, m_epa.contactB, colors::kContact, 3.0f);
        view.DrawPoint(m_epa.contactA, colors::kContact, 5.0f);
        view.DrawPoint(m_epa.contactB, colors::kContact, 5.0f);

        char label[64];
        std::snprintf(label, sizeof(label), "depth %.3f", m_epa.depth);
        view.DrawText(m_epa.contactA + math::Vector3(0.1f, 0.2f, 0.0f), colors::kContact, label);
    }

    void ConfigureView(Viewport2D& view) override
    {
        view.SetPlane(ViewPlane::XY);
        view.SetFocus({0.5f, 0.0f, 0.0f}, 100.0f);
    }

private:
    static bool ShapeCombo(const char* label, ShapeKind& kind)
    {
        int index = (kind == ShapeKind::Sphere) ? 0 : 1;
        if (!ImGui::Combo(label, &index, "球\0箱\0")) return false;
        kind = (index == 0) ? ShapeKind::Sphere : ShapeKind::Box;
        return true;
    }

    static void DrawShape(Viewport2D& view, ShapeKind kind, const Sphere& sphere, const Box& box,
                          ImU32 color)
    {
        if (kind == ShapeKind::Sphere) view.DrawCircle(sphere.center, sphere.radius, color);
        else                            view.DrawBox(box.center, box.halfExtents, color);
    }

    math::Vector3 CenterB() const
    {
        return (m_kindB == ShapeKind::Sphere) ? m_b.center : m_boxB.center;
    }

    void SetCenterB(const math::Vector3& center)
    {
        m_b.center    = center;
        m_boxB.center = center;
    }

    void Evaluate()
    {
        const void*        shapeA = (m_kindA == ShapeKind::Sphere) ? static_cast<const void*>(&m_a)
                                                                   : static_cast<const void*>(&m_boxA);
        const void*        shapeB = (m_kindB == ShapeKind::Sphere) ? static_cast<const void*>(&m_b)
                                                                   : static_cast<const void*>(&m_boxB);
        physics::SupportFn fnA = (m_kindA == ShapeKind::Sphere) ? &Sphere::Support : &Box::Support;
        physics::SupportFn fnB = (m_kindB == ShapeKind::Sphere) ? &Sphere::Support : &Box::Support;

        m_gjk = physics::GJK_Intersect(shapeA, fnA, shapeB, fnB);
        m_epa = m_gjk.intersects
                    ? physics::EPA_GetContactInfo(shapeA, fnA, shapeB, fnB, m_gjk.simplex)
                    : physics::EPAResult{};
        m_gap = m_gjk.intersects ? physics::GJKResult{}
                                 : physics::GJK_Distance(shapeA, fnA, shapeB, fnB);
    }

    Sphere    m_a;
    Sphere    m_b;
    Box       m_boxA;
    Box       m_boxB;
    ShapeKind m_kindA = ShapeKind::Sphere;
    ShapeKind m_kindB = ShapeKind::Sphere;

    bool  m_orbit = false;
    float m_angle = 0.0f;

    physics::GJKResult m_gjk;
    physics::EPAResult m_epa;
    physics::GJKResult m_gap;   ///< 非交差時の隙間 (GJK_Distance)
};

} // namespace

std::unique_ptr<BenchScene> MakeContactScene() { return std::make_unique<ContactScene>(); }

} // namespace fbzz::bench
