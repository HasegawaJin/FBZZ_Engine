/// @file    ConvexHullScene.cpp
/// @brief   Quickhull が作った凸包の «形» と、GJK が使うサポート点を目で見る場面。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// ConvexHullTests は «立方体なら頂点 8 個» のような数を固定している。だが凸包の失敗は
/// 数では出ない ── 面が裏返る、点を 1 個だけ包み損ねる、退化した点群 (同一平面・一直線)
/// で薄い包みが破綻する。どれも «そのうち GJK が変な法線を返す» という形でしか表に出ない。
///
/// ここでは入力点・包んだ頂点・面の稜線を重ねて描き、**すべての入力点が包みの内側にあるか**
/// をその場で数える。GJK が実際に呼ぶ SupportPoint も同時に叩き、方向を回して
/// «常に一番遠い頂点が選ばれているか» を見る。
#include "Scenes.hpp"

#include <Physics/ConvexHullCollider.hpp>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace fbzz::bench {

namespace {

/// 点群の作り方。«うまくいく形» だけでなく退化した形を必ず選べるようにする ──
/// 凸包が落ちるのはいつもそちら側。
enum class CloudShape {
    RandomBox = 0,
    SphereShell,
    CubeCorners,
    Coplanar,   ///< 全点が同一平面。体積 0
    Collinear,  ///< 全点が一直線。面が作れない
    Duplicated, ///< 同じ点の繰り返し。縮退の極限
};

/// 決定論的な乱数。場面を作り直すたびに違う点群が出ると «直したか» が判断できない。
class Lcg {
public:
    explicit Lcg(uint32_t seed) : m_state(seed * 2654435761u + 1u) {}
    float Next()
    {
        m_state = m_state * 1664525u + 1013904223u;
        return static_cast<float>(m_state >> 8) / static_cast<float>(1u << 24);
    }
    float Signed() { return Next() * 2.0f - 1.0f; }

private:
    uint32_t m_state;
};

std::vector<math::Vector3> MakeCloud(CloudShape shape, int count, uint32_t seed)
{
    Lcg rng(seed);
    std::vector<math::Vector3> points;
    points.reserve(static_cast<std::size_t>(count));

    switch (shape) {
    case CloudShape::SphereShell:
        for (int i = 0; i < count; ++i) {
            const math::Vector3 v{ rng.Signed(), rng.Signed(), rng.Signed() };
            const float len = v.Length();
            points.push_back(len > 1e-4f ? v * (1.5f / len) : math::Vector3{ 1.5f, 0.0f, 0.0f });
        }
        break;
    case CloudShape::CubeCorners:
        for (int i = 0; i < count; ++i) {
            // 角 8 点を繰り返しつつ、内側の点も混ぜる。内側の点は包みに現れてはならない。
            if (i < 8)
                points.push_back({ (i & 1) ? 1.5f : -1.5f,
                                   (i & 2) ? 1.5f : -1.5f,
                                   (i & 4) ? 1.5f : -1.5f });
            else
                points.push_back({ rng.Signed(), rng.Signed(), rng.Signed() });
        }
        break;
    case CloudShape::Coplanar:
        for (int i = 0; i < count; ++i)
            points.push_back({ rng.Signed() * 1.6f, rng.Signed() * 1.6f, 0.0f });
        break;
    case CloudShape::Collinear:
        for (int i = 0; i < count; ++i) points.push_back({ rng.Signed() * 1.8f, 0.0f, 0.0f });
        break;
    case CloudShape::Duplicated:
        for (int i = 0; i < count; ++i) points.push_back({ 0.7f, -0.3f, 0.2f });
        break;
    default:
        for (int i = 0; i < count; ++i)
            points.push_back({ rng.Signed() * 1.6f, rng.Signed() * 1.2f, rng.Signed() * 1.4f });
        break;
    }
    return points;
}

class ConvexHullScene final : public BenchScene {
public:
    ConvexHullScene() { Reset(); }

    const char* Name() const override { return "凸包: Quickhull とサポート点"; }

    const char* WhatToLookFor() const override
    {
        return "小さい灰点が入力、青点が包みの頂点、細線が面の稜線、黄矢印が探索方向、\n"
               "その先の赤点が SupportPoint が返した頂点。\n"
               "・«包みの外に出た入力点» が 0 のままであること。1 個でも出たら Quickhull の欠陥\n"
               "・内側にある入力点が青くならないこと (無駄な頂点を抱えていない)\n"
               "・方向を回したとき、赤点が «その向きで一番遠い頂点» から動かないこと。\n"
               "  途中で内側の点へ飛ぶと GJK が誤った距離を出す\n"
               "・同一平面 / 一直線 / 同一点 を選んでも落ちず、頂点数が 0 にならないこと。\n"
               "  退化した点群は実データ (板ポリのメッシュ、潰れたスケール) で普通に来る\n"
               "・点数を頂点上限 (64) より増やしても、上限で頭打ちになるだけで形が壊れないこと\n"
               "回転させると奥行きが読める。止めて方向を回すとサポート点だけを追える。";
    }

    void Reset() override
    {
        m_shape     = CloudShape::RandomBox;
        m_count     = 40;
        m_seed      = 7;
        m_spin      = true;
        m_angle     = 0.0f;
        m_dirAngle  = 0.6f;
        m_showInput = true;
        Rebuild();
    }

    void Simulate(float dt) override
    {
        if (m_spin) {
            m_angle += dt * 0.6f;
            Transform();
        }
    }

    void DrawControls() override
    {
        bool dirty = false;

        int shape = static_cast<int>(m_shape);
        if (ImGui::Combo("点群", &shape,
                         "箱の中の乱数\0球面\0立方体の角 + 内側\0同一平面\0一直線\0同一点\0")) {
            m_shape = static_cast<CloudShape>(shape);
            dirty   = true;
        }
        dirty |= ImGui::SliderInt("点数", &m_count, 1, 200);
        dirty |= ImGui::SliderInt("乱数の種", &m_seed, 0, 64);
        if (dirty) Rebuild();

        ImGui::Separator();
        ImGui::Checkbox("回す", &m_spin);
        if (!m_spin && ImGui::SliderFloat("角度", &m_angle, 0.0f, 6.283f, "%.2f rad")) Transform();
        ImGui::Checkbox("入力点を表示", &m_showInput);
        ImGui::SliderFloat("探索方向", &m_dirAngle, 0.0f, 6.283f, "%.2f rad");

        ImGui::Separator();
        ImGui::Text("入力 %zu 点 → 包みの頂点 %zu / 面 %zu",
                    m_points.size(), m_worldVerts.size(), m_faces.size());
        if (static_cast<int>(m_worldVerts.size()) >= physics::ConvexHullCollider::MAX_HULL_VERTS)
            ImGui::TextColored({ 1.0f, 0.72f, 0.36f, 1.0f },
                               "頂点上限 %d に達している (これ以上は取りこぼす)",
                               physics::ConvexHullCollider::MAX_HULL_VERTS);

        if (m_outsideCount > 0)
            ImGui::TextColored({ 1.0f, 0.36f, 0.41f, 1.0f },
                               "包みの外に出た入力点 %d 個 (最大 %.4f m)",
                               m_outsideCount, m_worstOutside);
        else
            ImGui::TextColored({ 0.48f, 0.90f, 0.55f, 1.0f }, "すべての入力点が包みの内側");

        if (m_faces.empty())
            ImGui::TextColored({ 0.74f, 0.77f, 0.81f, 1.0f },
                               "面が 0 (退化した点群。GJK はサポート点だけで解く)");

        const math::Vector3 support = SupportPoint();
        ImGui::Text("サポート点 (%+.3f, %+.3f, %+.3f)", support.x, support.y, support.z);
    }

    void Draw(Viewport2D& view) override
    {
        view.DrawGrid(0.5f);

        if (m_showInput)
            for (const math::Vector3& p : m_rotatedPoints)
                view.DrawPoint(p, colors::kGrid, 2.5f);

        // 面の稜線。潰れた点群では 0 本になる ── それ自体が見たい情報。
        for (const std::array<uint32_t, 3>& face : m_faces) {
            if (face[0] >= m_worldVerts.size() || face[1] >= m_worldVerts.size()
                || face[2] >= m_worldVerts.size())
                continue;
            view.DrawLine(m_worldVerts[face[0]], m_worldVerts[face[1]], colors::kHint, 1.0f);
            view.DrawLine(m_worldVerts[face[1]], m_worldVerts[face[2]], colors::kHint, 1.0f);
            view.DrawLine(m_worldVerts[face[2]], m_worldVerts[face[0]], colors::kHint, 1.0f);
        }

        for (const math::Vector3& v : m_worldVerts) view.DrawPoint(v, colors::kBody, 4.0f);

        // 包み損ねた点は «欠陥そのもの» なので、他より目立たせる。
        for (const math::Vector3& p : m_outsidePoints) view.DrawCircle(p, 0.08f, colors::kContact,
                                                                      false, 2.0f);

        // GJK が実際に呼ぶ経路。方向と返り値を並べて描く。
        const math::Vector3 dir = Direction();
        view.DrawArrow({ 0.0f, 0.0f, 0.0f }, dir * 2.6f, colors::kQuery, 2.0f);
        view.DrawPoint(SupportPoint(), colors::kContact, 6.0f);
    }

    void ConfigureView(Viewport2D& view) override
    {
        view.SetPlane(ViewPlane::XY);
        view.SetFocus({ 0.0f, 0.0f, 0.0f }, 120.0f);
    }

private:
    math::Vector3 Direction() const
    {
        return { std::cos(m_dirAngle), std::sin(m_dirAngle), 0.0f };
    }

    math::Vector3 SupportPoint() const
    {
        return m_hull ? m_hull->SupportPoint(Direction()) : math::Vector3::ZERO;
    }

    void Rebuild()
    {
        m_points = MakeCloud(m_shape, m_count, static_cast<uint32_t>(m_seed));
        m_hull   = std::make_unique<physics::ConvexHullCollider>(m_points);
        Transform();
    }

    /// 包みと入力点を同じ回転で回す。片方だけ回すと «包み損ね» が偽で出る。
    void Transform()
    {
        if (!m_hull) return;

        const math::Quaternion rot =
            math::Quaternion::FromAxisAngle({ 0.32f, 0.90f, 0.30f }, m_angle);
        m_hull->Update(math::Vector3::ZERO, rot);

        m_worldVerts = m_hull->GetWorldVertices();
        m_faces      = m_hull->GetFaces();

        m_rotatedPoints.clear();
        m_rotatedPoints.reserve(m_points.size());
        for (const math::Vector3& p : m_points) m_rotatedPoints.push_back(rot * p);

        CheckContainment();
    }

    /// 入力点がすべて包みの内側にあるか。面の向きは重心を基準に外向きへ揃える
    /// (Quickhull の巻き方に依存させると «巻きが逆でも通る» 検査になってしまう)。
    void CheckContainment()
    {
        m_outsidePoints.clear();
        m_outsideCount = 0;
        m_worstOutside = 0.0f;
        if (m_faces.empty() || m_worldVerts.empty()) return;

        math::Vector3 centroid = math::Vector3::ZERO;
        for (const math::Vector3& v : m_worldVerts) centroid = centroid + v;
        centroid = centroid * (1.0f / static_cast<float>(m_worldVerts.size()));

        // 面の厚みは点群の広がりに対する比で見る。絶対値だと大きな形ほど厳しくなる。
        float extent = 0.0f;
        for (const math::Vector3& v : m_worldVerts)
            extent = (std::max)(extent, (v - centroid).Length());
        const float tolerance = (std::max)(extent, 1.0f) * 1e-3f;

        for (const math::Vector3& p : m_rotatedPoints) {
            float worst = 0.0f;
            for (const std::array<uint32_t, 3>& face : m_faces) {
                if (face[0] >= m_worldVerts.size() || face[1] >= m_worldVerts.size()
                    || face[2] >= m_worldVerts.size())
                    continue;
                const math::Vector3& a = m_worldVerts[face[0]];
                const math::Vector3  e1 = m_worldVerts[face[1]] - a;
                const math::Vector3  e2 = m_worldVerts[face[2]] - a;

                math::Vector3 normal = math::Vector3::Cross(e1, e2);
                const float   length = normal.Length();
                if (length < 1e-6f) continue;
                normal = normal * (1.0f / length);
                if (math::Vector3::Dot(normal, a - centroid) < 0.0f) normal = normal * -1.0f;

                worst = (std::max)(worst, math::Vector3::Dot(normal, p - a));
            }
            if (worst <= tolerance) continue;
            ++m_outsideCount;
            m_worstOutside = (std::max)(m_worstOutside, worst);
            m_outsidePoints.push_back(p);
        }
    }

    std::unique_ptr<physics::ConvexHullCollider> m_hull;
    std::vector<math::Vector3>                   m_points;
    std::vector<math::Vector3>                   m_rotatedPoints;
    std::vector<math::Vector3>                   m_worldVerts;
    std::vector<std::array<uint32_t, 3>>         m_faces;
    std::vector<math::Vector3>                   m_outsidePoints;

    CloudShape m_shape     = CloudShape::RandomBox;
    int        m_count     = 40;
    int        m_seed      = 7;
    bool       m_spin      = true;
    float      m_angle     = 0.0f;
    float      m_dirAngle  = 0.6f;
    bool       m_showInput = true;
    int        m_outsideCount = 0;
    float      m_worstOutside = 0.0f;
};

} // namespace

std::unique_ptr<BenchScene> MakeConvexHullScene() { return std::make_unique<ConvexHullScene>(); }

} // namespace fbzz::bench
