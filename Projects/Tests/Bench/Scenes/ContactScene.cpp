/// @file    ContactScene.cpp
/// @brief   NarrowPhase が返す接触 (法線・深さ・マニフォールド) を、形状を動かしながら目で見る場面。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// @note 以前は «球と箱を模した支持関数» で GJK/EPA を直接叩いていたが、それではエンジンが実際にどう答えるかを見ておらず、解析解や dispatch の不具合を検出できなかった。
/// @note 実際に見つかった不具合: 球が箱内部で真上へ押し出す、カプセル貫通で «下へ抜ける» と誤判定、ConvexHull×HeightField が dispatch から漏れる、など数値でも掴みにくい症状ばかりだった。
/// @note ゲームと同じ経路 (実コライダー + NarrowPhase) を通して見る。
#include "Scenes.hpp"

#include <Physics/AABBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/CollisionPair.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/CylinderCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include <Physics/PhysicsSolver.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/SphereCollider.hpp>

#include <Math/MathUtils.hpp>

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace fbzz::bench {

namespace {

enum class ShapeKind { Sphere = 0, Box, OrientedBox, Capsule, Cylinder, Hull };

constexpr const char* kShapeItems = "球\0箱 (AABB)\0箱 (OBB)\0カプセル\0円柱\0凸包\0";

std::vector<math::Vector3> CubePoints()
{
    return {
        { -1.0f, -1.0f, -1.0f }, { 1.0f, -1.0f, -1.0f }, { -1.0f, 1.0f, -1.0f },
        { 1.0f, 1.0f, -1.0f },   { -1.0f, -1.0f, 1.0f }, { 1.0f, -1.0f, 1.0f },
        { -1.0f, 1.0f, 1.0f },   { 1.0f, 1.0f, 1.0f },
    };
}

std::unique_ptr<physics::Collider> MakeCollider(ShapeKind kind)
{
    switch (kind) {
    case ShapeKind::Sphere:      return std::make_unique<physics::SphereCollider>(1.0f);
    case ShapeKind::Box:         return std::make_unique<physics::AABBCollider>(
                                            math::Vector3(1.0f, 1.0f, 1.0f));
    case ShapeKind::OrientedBox: return std::make_unique<physics::OBBCollider>(
                                            math::Vector3(1.0f, 1.0f, 1.0f));
    case ShapeKind::Capsule:     return std::make_unique<physics::CapsuleCollider>(0.5f, 1.0f);
    case ShapeKind::Cylinder:    return std::make_unique<physics::CylinderCollider>(1.0f, 1.0f);
    case ShapeKind::Hull:        return std::make_unique<physics::ConvexHullCollider>(CubePoints());
    }
    return nullptr;
}

/// 剛体 1 個ぶん。ゲームと同じく «コライダー + 剛体 + ColliderInstance» の 3 点セットで持つ。
/// 法線の向き合わせは剛体の位置関係で決まるので、剛体を省くと最後の一手間が抜ける。
struct Actor {
    std::unique_ptr<physics::Collider> collider;
    physics::RigidBody                 body;
    ShapeKind                          kind = ShapeKind::Sphere;
    math::Vector3                      position;
    float                              angleDegrees = 0.0f;

    void Rebuild(ShapeKind newKind)
    {
        kind     = newKind;
        collider = MakeCollider(kind);
        body.SetMass(1.0f);
        Sync();
    }

    void Sync()
    {
        const math::Quaternion rotation = math::Quaternion::FromAxisAngle(
            math::Vector3::FORWARD, math::ToRad(angleDegrees));
        collider->Update(position, rotation);
        body.SetPosition(position);
        body.SetRotation(rotation);
    }

    physics::ColliderInstance Instance()
    {
        physics::ColliderInstance instance;
        instance.collider = collider.get();
        instance.body     = &body;
        instance.material = &physics::PhysicsMaterial::Default;
        return instance;
    }
};

class ContactScene final : public BenchScene {
public:
    ContactScene() { Reset(); }

    const char* Name() const override { return "NarrowPhase: 接触の法線と深さ"; }

    const char* WhatToLookFor() const override
    {
        return "細い枠は «エンジンが持っているワールド AABB»、緑の矢印が接触法線 "
               "(B から A へ押し出す向き)、赤い線が貫通量。\n"
               "・矢印が «A を B から遠ざける» 向きを指していること。逆を向いていたら "
               "その形状の組は物体を相手へ吸い込む\n"
               "・B をゆっくり動かしたとき、法線が滑らかに向きを変えること "
               "(箱どうしの面の切り替わりで飛ぶのは正常)\n"
               "・深く重ねたとき、押し出す向きが «一番浅い抜け道» であること。"
               "真上に固定されたり、明後日の向きへ倒れたりしないこと\n"
               "・箱どうしを面で当てたとき、接触点が複数出ること (1 点だと接地面で揺れる)\n"
               "・B を回したとき、OBB とカプセルの判定が向きに追従すること\n"
               "・«AABB は重なっているのに接触 0» が出たら、その組が dispatch から漏れている";
    }

    void Reset() override
    {
        m_a.position     = math::Vector3::ZERO;
        m_a.angleDegrees = 0.0f;
        m_a.Rebuild(ShapeKind::Sphere);

        m_b.position     = { 1.4f, 0.4f, 0.0f };
        m_b.angleDegrees = 0.0f;
        m_b.Rebuild(ShapeKind::Sphere);

        m_orbit = false;
        m_angle = 0.0f;
        Evaluate();
    }

    void Simulate(float dt) override
    {
        if (m_orbit) {
            m_angle += dt * 0.6f;
            constexpr float kRadius = 1.4f;
            m_b.position = { std::cos(m_angle) * kRadius, std::sin(m_angle) * kRadius, 0.0f };
            m_b.Sync();
        }
        Evaluate();
    }

    void DetectAnomalies(AnomalyLog& log) override
    {
        const physics::AABB boundsA = m_a.collider->GetAABB();
        const physics::AABB boundsB = m_b.collider->GetAABB();
        const bool          overlap = boundsA.Overlaps(boundsB);

        /// @note «AABB は重なるのに接触 0» はここに入れない。球を斜めに置くだけで普通に起きるため、いつも点く警告は読まれなくなる。人の判断が要る話は設定欄の説明文に残し、ここには白黒つくものだけ置く。
        if (m_contacts.empty()) return;

        /// @note AABB が離れていれば形も必ず離れている。ここで接触が出るのは偽陽性。
        log.ReportIf(!overlap, Severity::Error,
                     "AABB が離れているのに接触が %zu 点出ている", m_contacts.size());

        const math::Vector3 bToA    = m_a.position - m_b.position;
        const float         maxDepth = boundsA.Extents().Length() + boundsB.Extents().Length();

        for (const physics::ContactPoint& contact : m_contacts) {
            if (!log.CheckFinite("接触点", contact.point, 1.0e4f)) continue;
            if (!log.CheckFinite("接触法線", contact.normal, 1.0e2f)) continue;

            const float length = contact.normal.Length();
            log.ReportIf(std::fabs(length - 1.0f) > 1.0e-3f, Severity::Error,
                         "法線が単位長でない (|n| = %.5f)", length);

            /// @note 逆を向いた法線は、押し出しではなく相手への吸い込みになる。
            log.ReportIf(math::Vector3::Dot(contact.normal, bToA) < 0.0f, Severity::Error,
                         "法線が B→A の逆を向いている (%+.3f, %+.3f, %+.3f)",
                         contact.normal.x, contact.normal.y, contact.normal.z);

            log.ReportIf(contact.depth < 0.0f, Severity::Error,
                         "貫通量が負 (%.4f m)", contact.depth);
            log.ReportIf(contact.depth > maxDepth, Severity::Error,
                         "貫通量 %.4f m が形の大きさ (%.4f m) を超えている",
                         contact.depth, maxDepth);
        }
    }

    void DrawControls() override
    {
        bool dirty = false;

        int kindA = static_cast<int>(m_a.kind);
        if (ImGui::Combo("A の形", &kindA, kShapeItems)) {
            m_a.Rebuild(static_cast<ShapeKind>(kindA));
            dirty = true;
        }
        int kindB = static_cast<int>(m_b.kind);
        if (ImGui::Combo("B の形", &kindB, kShapeItems)) {
            m_b.Rebuild(static_cast<ShapeKind>(kindB));
            dirty = true;
        }

        ImGui::Separator();
        float position[2]{ m_b.position.x, m_b.position.y };
        if (ImGui::SliderFloat2("B の位置", position, -4.0f, 4.0f, "%.3f")) {
            m_b.position = { position[0], position[1], 0.0f };
            dirty = true;
        }
        /// @note 回転を触れないと OBB とカプセルの «向きに追従するか» が一切見えない。
        if (ImGui::SliderFloat("B の角度", &m_b.angleDegrees, -180.0f, 180.0f, "%.1f 度"))
            dirty = true;
        if (ImGui::SliderFloat("A の角度", &m_a.angleDegrees, -180.0f, 180.0f, "%.1f 度"))
            dirty = true;
        ImGui::Checkbox("A のまわりを回す", &m_orbit);

        /// @note 触った結果をこのフレームのうちに反映する。次フレームまで待つと、
        ///       スライダーを動かしている間だけ «1 つ前の答え» を見ることになる。
        if (dirty) Sync();

        ImGui::Separator();
        const bool boundsOverlap = m_a.collider->GetAABB().Overlaps(m_b.collider->GetAABB());
        ImGui::Text("AABB の重なり: %s", boundsOverlap ? "あり" : "なし");

        if (m_contacts.empty()) {
            if (boundsOverlap) {
                /// @note BroadPhase を通る配置なのに接触が出ない = 実際に起きた不具合の形。
                ImGui::TextColored({ 1.0f, 0.36f, 0.41f, 1.0f },
                                   "接触なし (AABB は重なっている)");
                ImGui::TextWrapped("形状が離れているならこれで正しい。食い込んで見えるなら、"
                                   "この組が NarrowPhase の dispatch から漏れている。");
            } else {
                ImGui::TextColored({ 0.74f, 0.77f, 0.81f, 1.0f }, "接触なし");
            }
            return;
        }

        ImGui::TextColored({ 0.48f, 0.90f, 0.55f, 1.0f }, "接触 %d 点",
                           static_cast<int>(m_contacts.size()));
        if (ImGui::BeginTable("##contacts", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("法線");
            ImGui::TableSetupColumn("深さ");
            ImGui::TableSetupColumn("向き");
            ImGui::TableHeadersRow();

            const math::Vector3 bToA = m_a.position - m_b.position;
            for (const physics::ContactPoint& contact : m_contacts) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%+.3f, %+.3f", contact.normal.x, contact.normal.y);
                ImGui::TableNextColumn();
                ImGui::Text("%.4f", contact.depth);
                ImGui::TableNextColumn();
                /// @note 押し出す向きが «B から A» を向いているか。数値で見えないと判断できない。
                const float agreement = math::Vector3::Dot(contact.normal, bToA);
                if (agreement >= 0.0f) ImGui::TextUnformatted("B→A");
                else ImGui::TextColored({ 1.0f, 0.36f, 0.41f, 1.0f }, "逆向き");
            }
            ImGui::EndTable();
        }
    }

    void Draw(Viewport2D& view) override
    {
        view.DrawGrid(0.5f);

        DrawActor(view, m_a, colors::kBody);
        DrawActor(view, m_b, colors::kBodyAlt);

        for (const physics::ContactPoint& contact : m_contacts) {
            /// @note 矢印の長さは «見える» 固定倍率。深さは赤い線の長さで別に見せる。
            view.DrawArrow(contact.point, contact.point + contact.normal * 1.0f,
                           colors::kNormal, 2.5f);
            view.DrawLine(contact.point, contact.point - contact.normal * contact.depth,
                          colors::kContact, 3.0f);
            view.DrawPoint(contact.point, colors::kContact, 5.0f);
        }

        if (m_contacts.size() == 1) {
            char label[64];
            std::snprintf(label, sizeof(label), "depth %.3f", m_contacts.front().depth);
            view.DrawText(m_contacts.front().point + math::Vector3(0.1f, 0.2f, 0.0f),
                          colors::kContact, label);
        }
    }

    void ConfigureView(Viewport2D& view) override
    {
        view.SetPlane(ViewPlane::XY);
        view.SetFocus({ 0.5f, 0.0f, 0.0f }, 100.0f);
    }

private:
    /// 形が読める最低限だけ描く。枠は «エンジンが持っている» ワールド AABB なので、
    /// ここがずれていればコライダーの Transform 同期そのものが疑わしい。
    static void DrawActor(Viewport2D& view, const Actor& actor, ImU32 color)
    {
        const physics::AABB bounds = actor.collider->GetAABB();
        view.DrawBox(bounds.Center(), bounds.Extents(), colors::kHint, false, 1.0f);

        switch (actor.kind) {
        case ShapeKind::Sphere: {
            /// @note 半径はコライダーから読む。描画と判定が別の値を見ていると、
            ///       «絵では当たっていないのに接触が出る» という嘘の疑いを生む。
            const auto& sphere = static_cast<const physics::SphereCollider&>(*actor.collider);
            view.DrawCircle(bounds.Center(), sphere.m_radius, color);
            break;
        }
        case ShapeKind::Capsule: {
            const auto& capsule = static_cast<const physics::CapsuleCollider&>(*actor.collider);
            view.DrawCircle(capsule.GetSegmentStart(), capsule.m_radius, color);
            view.DrawCircle(capsule.GetSegmentEnd(), capsule.m_radius, color);
            view.DrawLine(capsule.GetSegmentStart(), capsule.GetSegmentEnd(), color, 2.0f);
            break;
        }
        case ShapeKind::Cylinder: {
            const auto& cylinder = static_cast<const physics::CylinderCollider&>(*actor.collider);
            view.DrawLine(cylinder.GetSegmentStart(), cylinder.GetSegmentEnd(), color, 2.0f);
            view.DrawCircle(cylinder.GetCenter(), cylinder.m_radius, color, false, 2.0f);
            break;
        }
        case ShapeKind::OrientedBox: {
            const auto& box = static_cast<const physics::OBBCollider&>(*actor.collider);
            /// @note 回した箱は AABB では読めない。角そのものを打つ。
            for (const math::Vector3& corner : box.GetCorners())
                view.DrawPoint(corner, color, 3.0f);
            break;
        }
        case ShapeKind::Hull: {
            const auto& hull = static_cast<const physics::ConvexHullCollider&>(*actor.collider);
            for (const math::Vector3& vertex : hull.GetWorldVertices())
                view.DrawPoint(vertex, color, 3.0f);
            break;
        }
        case ShapeKind::Box:
            view.DrawBox(bounds.Center(), bounds.Extents(), color, false, 2.0f);
            break;
        }
    }

    void Sync()
    {
        m_a.Sync();
        m_b.Sync();
        Evaluate();
    }

    void Evaluate()
    {
        m_a.Sync();
        m_b.Sync();

        m_instanceA = m_a.Instance();
        m_instanceB = m_b.Instance();

        m_pairs.clear();
        m_pairs.push_back({ &m_instanceA, &m_instanceB });

        m_contacts.clear();
        m_solver.NarrowPhase(m_pairs, m_contacts);
    }

    physics::PhysicsSolver              m_solver;
    Actor                               m_a;
    Actor                               m_b;
    physics::ColliderInstance           m_instanceA;
    physics::ColliderInstance           m_instanceB;
    std::vector<physics::CollisionPair> m_pairs;
    std::vector<physics::ContactPoint>  m_contacts;

    bool  m_orbit = false;
    float m_angle = 0.0f;
};

} // namespace

std::unique_ptr<BenchScene> MakeContactScene() { return std::make_unique<ContactScene>(); }

} // namespace fbzz::bench
