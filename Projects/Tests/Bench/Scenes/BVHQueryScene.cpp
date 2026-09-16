/// @file    BVHQueryScene.cpp
/// @brief   BVH のノード境界と問い合わせの当たり方を目で見る場面。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// BVHTests が «総当たりと同じ結果» を数値で固定している。ここで見たいのは
/// 木の «形» ── 分割が偏っていないか、葉の境界が無駄に重なっていないか。
/// 結果は正しくても形が悪い木は、地形の当たり判定だけが重い、という形で表に出る。
#include "Scenes.hpp"

#include <Physics/BVHNode.hpp>

#include <imgui.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <vector>

namespace fbzz::bench {

namespace {

/// XZ 平面に並べた格子状の三角形。地形メッシュの縮小版。
std::vector<physics::Triangle> BuildGrid(int columns, int rows)
{
    std::vector<physics::Triangle> triangles;
    triangles.reserve(static_cast<size_t>(columns * rows * 2));

    uint32_t index = 0;
    for (int z = 0; z < rows; ++z) {
        for (int x = 0; x < columns; ++x) {
            const float fx = static_cast<float>(x);
            const float fz = static_cast<float>(z);

            physics::Triangle lower;
            lower.v[0]   = {fx, 0.0f, fz};
            lower.v[1]   = {fx + 1.0f, 0.0f, fz};
            lower.v[2]   = {fx, 0.0f, fz + 1.0f};
            lower.normal = math::Vector3::UP;
            lower.index  = index++;
            triangles.push_back(lower);

            physics::Triangle upper;
            upper.v[0]   = {fx + 1.0f, 0.0f, fz};
            upper.v[1]   = {fx + 1.0f, 0.0f, fz + 1.0f};
            upper.v[2]   = {fx, 0.0f, fz + 1.0f};
            upper.normal = math::Vector3::UP;
            upper.index  = index++;
            triangles.push_back(upper);
        }
    }
    return triangles;
}

class BVHQueryScene final : public BenchScene {
public:
    BVHQueryScene() { Reset(); }

    const char* Name() const override { return "BVH: 木の形と問い合わせ"; }

    const char* WhatToLookFor() const override
    {
        return "灰色の枠が BVH ノードの境界、黄色が問い合わせ箱、赤が返ってきた三角形。\n"
               "・問い合わせ箱に «触れている» 三角形がすべて赤いこと (取りこぼしがない)\n"
               "・箱から明らかに離れた三角形が赤くないこと\n"
               "・葉の境界どうしが大きく重なっていないこと (重なるほど枝刈りが効かない)\n"
               "「葉だけ表示」を切って深さを上げると、分割の偏りが見える。";
    }

    void Reset() override
    {
        m_columns     = 8;
        m_rows        = 8;
        m_maxLeafTris = 4;
        m_queryCenter = {4.0f, 0.0f, 4.0f};
        m_queryHalf   = 1.2f;
        m_leafOnly    = true;
        Rebuild();
    }

    void Simulate(float) override {}

    void DrawControls() override
    {
        bool dirty = false;
        dirty |= ImGui::SliderInt("列", &m_columns, 1, 24);
        dirty |= ImGui::SliderInt("行", &m_rows, 1, 24);
        dirty |= ImGui::SliderInt("葉あたりの三角形数", &m_maxLeafTris, 1, 16);
        if (dirty) Rebuild();

        ImGui::Separator();
        float center[2]{m_queryCenter.x, m_queryCenter.z};
        if (ImGui::SliderFloat2("問い合わせ中心", center, -4.0f, 28.0f, "%.2f")) {
            m_queryCenter = {center[0], 0.0f, center[1]};
            RunQuery();
        }
        if (ImGui::SliderFloat("問い合わせ半径", &m_queryHalf, 0.1f, 8.0f, "%.2f")) RunQuery();

        ImGui::Separator();
        ImGui::Checkbox("葉だけ表示", &m_leafOnly);

        ImGui::Text("三角形 %zu 枚 / ノード %zu 個", m_tree.triangles.size(), m_tree.nodes.size());
        ImGui::Text("ヒット %zu 枚 (総当たり %zu 枚)", m_hits.size(), m_bruteForce.size());
        if (m_hits != m_bruteForce) {
            ImGui::TextColored({1.0f, 0.36f, 0.41f, 1.0f}, "総当たりと一致していない");
        }
    }

    void DetectAnomalies(AnomalyLog& log) override
    {
        // 枝刈りが結果を変えていないこと。BVH の契約はこれ 1 つに尽きる。
        for (const uint32_t index : m_bruteForce)
            log.ReportIf(m_hits.count(index) == 0, Severity::Error,
                         "三角形 %u を取りこぼした (総当たりでは当たる)", index);
        for (const uint32_t index : m_hits)
            log.ReportIf(m_bruteForce.count(index) == 0, Severity::Error,
                         "三角形 %u を誤って返した (総当たりでは当たらない)", index);

        // 木の不変条件。子が親からはみ出すと、親で枝を切った瞬間に取りこぼす。
        // 結果が合っている «今の» 問い合わせ位置では見えないので、木そのものを見る。
        const int nodeCount = static_cast<int>(m_tree.nodes.size());
        for (int i = 0; i < nodeCount; ++i) {
            const physics::BVHNode& node = m_tree.nodes[static_cast<size_t>(i)];

            if (node.IsLeaf()) {
                log.ReportIf(static_cast<int>(node.triIndices.size()) > m_maxLeafTris,
                             Severity::Warning, "葉 %d が三角形を %zu 枚持っている (上限 %d)",
                             i, node.triIndices.size(), m_maxLeafTris);
                for (const uint32_t index : node.triIndices)
                    log.ReportIf(!Contains(node.aabb, TriangleBounds(m_tree.triangles[index])),
                                 Severity::Error, "葉 %d の境界が三角形 %u を包んでいない",
                                 i, index);
                continue;
            }

            const int children[2]{node.left, node.right};
            for (const int child : children) {
                if (child < 0 || child >= nodeCount) {
                    log.Report(Severity::Error, "ノード %d の子 %d が範囲外", i, child);
                    continue;
                }
                log.ReportIf(!Contains(node.aabb, m_tree.nodes[static_cast<size_t>(child)].aabb),
                             Severity::Error, "ノード %d の境界が子 %d を包んでいない", i, child);
            }
        }
    }

    void Draw(Viewport2D& view) override
    {
        view.DrawGrid(1.0f);

        for (const physics::BVHNode& node : m_tree.nodes) {
            if (m_leafOnly && !node.IsLeaf()) continue;
            const math::Vector3 center  = node.aabb.Center();
            const math::Vector3 extents = node.aabb.Extents();
            view.DrawBox(center, {extents.x, extents.y, extents.z},
                         node.IsLeaf() ? colors::kHint : colors::kGrid, false, 1.0f);
        }

        for (const physics::Triangle& tri : m_tree.triangles) {
            const bool  hit   = m_hits.count(tri.index) != 0;
            const ImU32 color = hit ? colors::kContact : colors::kBody;
            view.DrawLine(tri.v[0], tri.v[1], color, hit ? 2.0f : 1.0f);
            view.DrawLine(tri.v[1], tri.v[2], color, hit ? 2.0f : 1.0f);
            view.DrawLine(tri.v[2], tri.v[0], color, hit ? 2.0f : 1.0f);
        }

        const math::Vector3 half{m_queryHalf, m_queryHalf, m_queryHalf};
        view.DrawBox(m_queryCenter, half, colors::kQuery, false, 2.5f);
    }

    void ConfigureView(Viewport2D& view) override
    {
        view.SetPlane(ViewPlane::XZ);
        view.SetFocus({4.0f, 0.0f, 4.0f}, 48.0f);
    }

private:
    physics::AABB QueryBox() const
    {
        const math::Vector3 half{m_queryHalf, m_queryHalf, m_queryHalf};
        return {m_queryCenter - half, m_queryCenter + half};
    }

    void Rebuild()
    {
        m_tree.Build(BuildGrid(m_columns, m_rows), m_maxLeafTris);
        RunQuery();
    }

    void RunQuery()
    {
        const physics::AABB query = QueryBox();

        m_hits.clear();
        m_tree.Query(query, [this](const physics::Triangle& tri) { m_hits.insert(tri.index); });

        // 総当たりと突き合わせ、枝刈りが結果を変えていないことをその場で示す。
        // 件数ではなく «どの三角形か» で持つ ── 取りこぼしと偽陽性が 1 枚ずつ起きると
        // 件数は一致したまま中身が入れ替わる。
        m_bruteForce.clear();
        for (const physics::Triangle& tri : m_tree.triangles)
            if (TriangleBounds(tri).Overlaps(query)) m_bruteForce.insert(tri.index);
    }

    static physics::AABB TriangleBounds(const physics::Triangle& tri)
    {
        physics::AABB bounds{tri.v[0], tri.v[0]};
        for (int k = 1; k < 3; ++k) bounds = bounds.Merge({tri.v[k], tri.v[k]});
        return bounds;
    }

    /// outer が inner を完全に包んでいるか。境界がぴったり重なる分割は正常なので、
    /// 浮動小数の丸め 1 つぶんだけ余裕を持たせる。
    static bool Contains(const physics::AABB& outer, const physics::AABB& inner)
    {
        constexpr float kSlack = 1.0e-4f;
        return inner.min.x >= outer.min.x - kSlack && inner.max.x <= outer.max.x + kSlack
            && inner.min.y >= outer.min.y - kSlack && inner.max.y <= outer.max.y + kSlack
            && inner.min.z >= outer.min.z - kSlack && inner.max.z <= outer.max.z + kSlack;
    }

    physics::BVHTree   m_tree;
    std::set<uint32_t> m_hits;
    std::set<uint32_t> m_bruteForce;

    int           m_columns     = 8;
    int           m_rows        = 8;
    int           m_maxLeafTris = 4;
    math::Vector3 m_queryCenter{4.0f, 0.0f, 4.0f};
    float         m_queryHalf = 1.2f;
    bool          m_leafOnly  = true;
};

} // namespace

std::unique_ptr<BenchScene> MakeBVHQueryScene() { return std::make_unique<BVHQueryScene>(); }

} // namespace fbzz::bench
