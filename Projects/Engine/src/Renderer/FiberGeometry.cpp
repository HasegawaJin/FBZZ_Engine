/// @file    FiberGeometry.cpp
/// @brief   継ぎ目を共有した辺ごとに根元固定の分割 Fin を生成する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <Engine/Renderer/FiberGeometry.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace fbzz::renderer {
namespace {
struct FiberEdge {
    uint32_t m_first = 0;
    uint32_t m_second = 0;
    math::Vector3 m_normals[2];
    uint32_t m_faces = 0;
};

bool FiniteFiberVector(math::Vector3 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace

bool BuildFiberFins(const Mesh& mesh, FiberFinMesh& out)
{
    if (mesh.isSkinned) {
        if (mesh.cpuSkinnedVertices.empty()) return false;
        Mesh surface;
        surface.cpuIndices = mesh.cpuIndices;
        for (const auto& v : mesh.cpuSkinnedVertices) {
            Vertex vertex;
            vertex.position = v.position;
            vertex.normal = v.normal;
            vertex.uv = v.uv;
            surface.cpuVertices.push_back(vertex);
        }
        FiberFinMesh result;
        if (!BuildFiberFins(surface, result)) return false;
        for (size_t i = 0; i < result.m_vertices.size(); ++i) {
            const auto& source = mesh.cpuSkinnedVertices[result.m_sourceVertices[i]];
            std::copy_n(source.boneIndices, 4, result.m_vertices[i].m_boneIndices);
            std::copy_n(source.boneWeights, 4, result.m_vertices[i].m_boneWeights);
        }
        out = std::move(result);
        return true;
    }
    /// @note 生成量の上限。巨大メッシュへの誤設定で Editor がメモリを使い切らないよう拒否する。
    constexpr size_t MAX_INPUT_INDICES = 600000;
    constexpr uint32_t FIN_SEGMENTS = 4;
    if (mesh.isSkinned || mesh.cpuIndices.size() % 3 != 0 || mesh.cpuIndices.size() > MAX_INPUT_INDICES)
        return false;

    for (const auto& vertex : mesh.cpuVertices) {
        if (!FiniteFiberVector(vertex.position) || !FiniteFiberVector(vertex.normal)
            || !std::isfinite(vertex.uv.x) || !std::isfinite(vertex.uv.y)) return false;
    }
    /// @note 溶接番号は同じ位置の頂点群が最初に現れた順。木構造の map を使わず整列した配列で一括に求める。
    const auto positionKey = [&mesh](uint32_t vertex) {
        const auto& p = mesh.cpuVertices[vertex].position;
        return std::array{ p.x, p.y, p.z };
    };
    std::vector<uint32_t> byPosition(mesh.cpuVertices.size());
    for (uint32_t i = 0; i < byPosition.size(); ++i) byPosition[i] = i;
    std::sort(byPosition.begin(), byPosition.end(), [&](uint32_t a, uint32_t b) {
        const auto ka = positionKey(a), kb = positionKey(b);
        return ka < kb || (!(kb < ka) && a < b);
    });
    std::vector<uint32_t> groupOf(mesh.cpuVertices.size());
    std::vector<uint32_t> groupFirst;
    for (size_t i = 0; i < byPosition.size(); ++i) {
        if (i == 0 || positionKey(byPosition[i - 1]) < positionKey(byPosition[i])) groupFirst.push_back(byPosition[i]);
        groupOf[byPosition[i]] = static_cast<uint32_t>(groupFirst.size() - 1);
    }
    std::vector<uint32_t> groupOrder(groupFirst.size());
    for (uint32_t i = 0; i < groupOrder.size(); ++i) groupOrder[i] = i;
    std::sort(groupOrder.begin(), groupOrder.end(), [&](uint32_t a, uint32_t b) { return groupFirst[a] < groupFirst[b]; });
    std::vector<uint32_t> groupId(groupFirst.size());
    for (uint32_t id = 0; id < groupOrder.size(); ++id) groupId[groupOrder[id]] = id;
    std::vector<uint32_t> vertexIds(mesh.cpuVertices.size());
    for (size_t i = 0; i < vertexIds.size(); ++i) vertexIds[i] = groupId[groupOf[i]];

    struct EdgeUse {
        std::pair<uint32_t, uint32_t> m_key;
        uint32_t m_first = 0;
        uint32_t m_second = 0;
        math::Vector3 m_normal;
    };
    std::vector<EdgeUse> uses;
    uses.reserve(mesh.cpuIndices.size());
    for (size_t i = 0; i < mesh.cpuIndices.size(); i += 3) {
        const uint32_t indices[] = { mesh.cpuIndices[i], mesh.cpuIndices[i + 1], mesh.cpuIndices[i + 2] };
        for (auto index : indices) if (index >= mesh.cpuVertices.size()) return false;
        const auto& a = mesh.cpuVertices[indices[0]].position;
        const auto& b = mesh.cpuVertices[indices[1]].position;
        const auto& c = mesh.cpuVertices[indices[2]].position;
        const auto normal = math::Vector3::Cross(b - a, c - a);
        if (!FiniteFiberVector(normal)) return false;
        if (normal.LengthSq() <= 1.0e-16f) continue;
        const auto unitNormal = normal.Normalized();
        for (uint32_t e = 0; e < 3; ++e) {
            const uint32_t first = indices[e], second = indices[(e + 1) % 3];
            uses.push_back({ std::minmax(vertexIds[first], vertexIds[second]), first, second, unitNormal });
        }
    }
    /// @note 安定整列で面の出現順を保ち、1 本目の面の頂点と法線を辺の代表にする。3 面以上は非多様体として拒否する。
    std::stable_sort(uses.begin(), uses.end(), [](const EdgeUse& a, const EdgeUse& b) { return a.m_key < b.m_key; });
    std::vector<FiberEdge> edges;
    for (size_t i = 0; i < uses.size();) {
        size_t end = i + 1;
        while (end < uses.size() && uses[end].m_key == uses[i].m_key) ++end;
        if (end - i > 2) return false;
        FiberEdge edge;
        edge.m_first = uses[i].m_first;
        edge.m_second = uses[i].m_second;
        edge.m_normals[0] = uses[i].m_normal;
        edge.m_normals[1] = uses[end - 1].m_normal;
        edge.m_faces = static_cast<uint32_t>(end - i);
        edges.push_back(edge);
        i = end;
    }

    FiberFinMesh result;
    result.m_vertices.reserve(edges.size() * (FIN_SEGMENTS + 1) * 2);
    result.m_indices.reserve(edges.size() * FIN_SEGMENTS * 6);
    for (const auto& edge : edges) {
        const Vertex* vertices[] = { &mesh.cpuVertices[edge.m_first], &mesh.cpuVertices[edge.m_second] };
        const auto uvDelta = vertices[1]->uv - vertices[0]->uv;
        const float uvLength = std::sqrt(uvDelta.x * uvDelta.x + uvDelta.y * uvDelta.y);
        const float edgeLength = (vertices[1]->position - vertices[0]->position).Length();
        if (!std::isfinite(uvLength) || !std::isfinite(edgeLength)) return false;
        const auto base = static_cast<uint32_t>(result.m_vertices.size());
        for (uint32_t h = 0; h <= FIN_SEGMENTS; ++h) {
            for (uint32_t side = 0; side < 2; ++side) {
                const auto& source = *vertices[side];
                const auto normal = source.normal.LengthSq() > 1.0e-12f
                    ? source.normal.Normalized() : edge.m_normals[0];
                result.m_vertices.push_back({ source.position, normal, source.uv,
                    edge.m_normals[0], edge.m_normals[1], static_cast<float>(h) / FIN_SEGMENTS,
                    side == 0 ? 0.0f : (uvLength > 1.0e-6f ? uvLength : edgeLength),
                    side == 0 ? math::Vector3{} : vertices[1]->position - vertices[0]->position });
                result.m_sourceVertices.push_back(side == 0 ? edge.m_first : edge.m_second);
            }
        }
        for (uint32_t h = 0; h < FIN_SEGMENTS; ++h) {
            const uint32_t p = base + h * 2;
            result.m_indices.insert(result.m_indices.end(), { p, p + 1, p + 2, p + 2, p + 1, p + 3 });
        }
    }
    out = std::move(result);
    return true;
}

int FiberLodShellCount(int maximum, int minimum, float distance, float nearDistance, float farDistance)
{
    maximum = std::clamp(maximum, 1, 64);
    minimum = std::clamp(minimum, 1, maximum);
    if (!std::isfinite(distance) || !std::isfinite(nearDistance) || !std::isfinite(farDistance)) return maximum;
    const float t = std::clamp((distance - nearDistance) / std::max(farDistance - nearDistance, 0.01f), 0.0f, 1.0f);
    const int level = t < 0.33f ? maximum : t < 0.66f ? (maximum + minimum) / 2 : minimum;
    return std::max(level, 1);
}

bool BuildFiberBlades(const Mesh& mesh, float density, float width, uint32_t seed, std::vector<FiberBladeRoot>& out,
                      bool densityFromVertexAlpha)
{
    if (mesh.isSkinned || mesh.cpuIndices.size() % 3 != 0 || mesh.cpuIndices.size() > 600000
        || !std::isfinite(density) || density <= 0 || !std::isfinite(width) || width <= 0) return false;
    std::vector<float> areas;
    float total = 0;
    for (size_t i = 0; i < mesh.cpuIndices.size(); i += 3) {
        for (size_t j = 0; j < 3; ++j) if (mesh.cpuIndices[i+j] >= mesh.cpuVertices.size()) return false;
        const auto& a = mesh.cpuVertices[mesh.cpuIndices[i]];
        const auto& b = mesh.cpuVertices[mesh.cpuIndices[i+1]];
        const auto& c = mesh.cpuVertices[mesh.cpuIndices[i+2]];
        if (!FiniteFiberVector(a.position) || !FiniteFiberVector(b.position) || !FiniteFiberVector(c.position)
            || !FiniteFiberVector(a.normal) || !FiniteFiberVector(b.normal) || !FiniteFiberVector(c.normal)) return false;
        total += math::Vector3::Cross(b.position-a.position, c.position-a.position).Length() * 0.5f;
        areas.push_back(total);
    }
    if (!std::isfinite(total) || total * density > static_cast<float>(FIBER_MAX_BLADES)) return false;
    std::vector<FiberBladeRoot> result;
    const auto count = static_cast<uint32_t>(total * density);
    result.reserve(count);
    auto random = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    for (uint32_t blade = 0; blade < count; ++blade) {
        const float target = random() * total;
        const size_t triangle = static_cast<size_t>(std::upper_bound(areas.begin(), areas.end(), target) - areas.begin());
        if (triangle >= areas.size()) continue;
        const auto& a = mesh.cpuVertices[mesh.cpuIndices[triangle*3]];
        const auto& b = mesh.cpuVertices[mesh.cpuIndices[triangle*3+1]];
        const auto& c = mesh.cpuVertices[mesh.cpuIndices[triangle*3+2]];
        const float u = std::sqrt(random()), v = random();
        const float wa = 1-u, wb = u*(1-v), wc = u*v;
        if (densityFromVertexAlpha) {
            /// @note A がほぼ 1 なら乱数を引かない。全面 1 のメッシュでは flag false と同じ根元になる。
            const float alpha = a.color.w*wa + b.color.w*wb + c.color.w*wc;
            if (alpha < 0.999f && random() >= alpha) continue;
        }
        const auto root = a.position*wa + b.position*wb + c.position*wc;
        auto normal = a.normal*wa + b.normal*wb + c.normal*wc;
        normal = normal.LengthSq() > 1.0e-12f ? normal.Normalized() : math::Vector3{0,1,0};
        const auto uv = a.uv*wa + b.uv*wb + c.uv*wc;
        const float angle = random()*6.2831853f;
        const auto axis = std::abs(normal.y) < 0.9f ? math::Vector3{0,1,0} : math::Vector3{1,0,0};
        const auto tangent = math::Vector3::Cross(normal,axis).Normalized();
        const auto bitangent = math::Vector3::Cross(normal,tangent);
        FiberBladeRoot bladeRoot;
        bladeRoot.m_position = root;
        bladeRoot.m_normal = normal;
        bladeRoot.m_u = uv.x;
        bladeRoot.m_v = uv.y;
        bladeRoot.m_height = 0.65f + random()*0.35f;
        bladeRoot.m_rank = random();
        /// @note 2 枚目のリボンは 90 度回した十字。回転後の幅ベクトルを CPU で確定し、GPU で三角関数を再評価しない。
        for (uint32_t ribbon=0; ribbon<2; ++ribbon) {
            const float turn = angle + static_cast<float>(ribbon)*1.5707963f;
            (ribbon == 0 ? bladeRoot.m_side0 : bladeRoot.m_side1)
                = (tangent*std::cos(turn)+bitangent*std::sin(turn))*width*0.5f;
        }
        result.push_back(bladeRoot);
    }
    out=std::move(result);
    return true;
}
} // namespace fbzz::renderer
