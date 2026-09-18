/// @file    MeshBuilder.cpp
/// @brief   MeshBuilder の形状生成と加工
/// @author  Hasegawa Jin
/// @date    2026-08-26
#include <Engine/Scene/MeshBuilder.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>

namespace fbzz::scene {

namespace {

constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = kPi * 2.0f;

/// 面の向きの規約 (この規約が全形状の頂点順序を決めている):
///   三角形 (a, b, c) が表を向くのは、法線 N が N = Cross(c - a, b - a) を満たすとき。
///   PrimitiveMesh::Quad が採っている «法線側から見て時計回りが表» と同じ意味。
///
/// 曲面 p(s, t) を格子で張るときは、外向き法線が N = Cross(∂p/∂s, ∂p/∂t) になるよう
/// パラメータの順番を選び、AddQuad(p(s0,t1), p(s1,t1), p(s1,t0), p(s0,t0)) で 1 マスを積む。
math::Vector3 FaceNormal(const math::Vector3& a, const math::Vector3& b, const math::Vector3& c)
{
    return math::Vector3::Cross(c - a, b - a);
}

/// 軸から正規直交基底を作る。Cross(up, axis) == right が成り立つ並びにしてある
/// (円筒・円錐・トーラスの外向き法線がこの関係に依存している)。
void AxisBasis(const math::Vector3& axis, math::Vector3& outAxis,
               math::Vector3& outRight, math::Vector3& outUp)
{
    outAxis = axis.NormalizedOr(math::Vector3::UP);
    const math::Vector3 reference =
        std::fabs(outAxis.y) > 0.99f ? math::Vector3::RIGHT : math::Vector3::UP;
    outRight = math::Vector3::Cross(reference, outAxis).NormalizedOr(math::Vector3::RIGHT);
    outUp    = math::Vector3::Cross(outAxis, outRight);
}

math::Vector3 TransformPoint(const math::Matrix4& matrix, const math::Vector3& point)
{
    return (matrix * math::Vector4(point, 1.0f)).XYZ();
}

math::Vector3 TransformDirection(const math::Matrix4& matrix, const math::Vector3& direction)
{
    return (matrix * math::Vector4(direction, 0.0f)).XYZ();
}

/// widths / radii は「点と同数」か「1 個 (全体で一定)」の 2 通りを許す。
float SampleWidth(std::span<const float> widths, std::size_t index, float fallback)
{
    if (widths.empty())      return fallback;
    if (widths.size() == 1)  return widths[0];
    return index < widths.size() ? widths[index] : widths.back();
}

} // namespace

/// @name 状態

void MeshBuilder::Clear()
{
    m_vertices.clear();
    m_indices.clear();
}

void MeshBuilder::Reserve(std::size_t vertexCount, std::size_t indexCount)
{
    m_vertices.reserve(vertexCount);
    m_indices.reserve(indexCount);
}

/// @name 手組み

uint32_t MeshBuilder::Push(const math::Vector3& position, const math::Vector3& normal,
                           const math::Vector2& uv)
{
    const uint32_t index = static_cast<uint32_t>(m_vertices.size());
    m_vertices.push_back(MeshVertex{ position, normal, math::Vector3::RIGHT, uv, m_color });
    return index;
}

uint32_t MeshBuilder::AddVertex(const MeshVertex& vertex)
{
    const uint32_t index = static_cast<uint32_t>(m_vertices.size());
    m_vertices.push_back(vertex);
    return index;
}

uint32_t MeshBuilder::AddVertex(const math::Vector3& position, const math::Vector3& normal,
                                const math::Vector2& uv)
{
    return Push(position, normal, uv);
}

uint32_t MeshBuilder::AddVertex(const math::Vector3& position, const math::Vector3& normal,
                                const math::Vector2& uv, const math::Vector4& color)
{
    const uint32_t index = static_cast<uint32_t>(m_vertices.size());
    m_vertices.push_back(MeshVertex{ position, normal, math::Vector3::RIGHT, uv, color });
    return index;
}

void MeshBuilder::AddTriangle(uint32_t a, uint32_t b, uint32_t c)
{
    m_indices.push_back(a);
    m_indices.push_back(b);
    m_indices.push_back(c);
}

void MeshBuilder::AddQuad(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    AddTriangle(a, b, c);
    AddTriangle(a, c, d);
}

/// @name 色

void MeshBuilder::ColorizeAll(const math::Vector4& color)
{
    for (MeshVertex& vertex : m_vertices) vertex.color = color;
}

void MeshBuilder::ColorizeRange(uint32_t firstVertex, uint32_t count, const math::Vector4& color)
{
    const uint32_t last = (std::min)(static_cast<uint32_t>(m_vertices.size()), firstVertex + count);
    for (uint32_t i = firstVertex; i < last; ++i) m_vertices[i].color = color;
}

/// @name 形状

void MeshBuilder::AddQuad(const math::Vector3& center, const math::Vector3& right,
                          const math::Vector3& up)
{
    const math::Vector3 normal = math::Vector3::Cross(right, up).NormalizedOr(math::Vector3::FORWARD);
    const math::Vector3 tangent = right.NormalizedOr(math::Vector3::RIGHT);

    const uint32_t base = static_cast<uint32_t>(m_vertices.size());
    Push(center - right + up, normal, { 0.0f, 0.0f });
    Push(center + right + up, normal, { 1.0f, 0.0f });
    Push(center + right - up, normal, { 1.0f, 1.0f });
    Push(center - right - up, normal, { 0.0f, 1.0f });
    for (uint32_t i = 0; i < 4; ++i) m_vertices[base + i].tangent = tangent;

    AddQuad(base, base + 1, base + 2, base + 3);
}

void MeshBuilder::AddPlane(const math::Vector3& center, const math::Vector3& right,
                           const math::Vector3& up, int subdivisions)
{
    const int cells = (std::max)(1, subdivisions);
    const math::Vector3 normal = math::Vector3::Cross(right, up).NormalizedOr(math::Vector3::FORWARD);
    const math::Vector3 tangent = right.NormalizedOr(math::Vector3::RIGHT);
    const uint32_t base = static_cast<uint32_t>(m_vertices.size());
    const int stride = cells + 1;

    for (int j = 0; j <= cells; ++j) {
        const float t = static_cast<float>(j) / static_cast<float>(cells);
        for (int i = 0; i <= cells; ++i) {
            const float s = static_cast<float>(i) / static_cast<float>(cells);
            const math::Vector3 position =
                center + right * (s * 2.0f - 1.0f) + up * (t * 2.0f - 1.0f);
            const uint32_t index = Push(position, normal, { s, 1.0f - t });
            m_vertices[index].tangent = tangent;
        }
    }

    for (int j = 0; j < cells; ++j) {
        for (int i = 0; i < cells; ++i) {
            const uint32_t s0t0 = base + static_cast<uint32_t>(j * stride + i);
            const uint32_t s1t0 = s0t0 + 1;
            const uint32_t s0t1 = s0t0 + static_cast<uint32_t>(stride);
            const uint32_t s1t1 = s0t1 + 1;
            AddQuad(s0t1, s1t1, s1t0, s0t0);
        }
    }
}

void MeshBuilder::AddBox(const math::Vector3& center, const math::Vector3& halfExtents)
{
    const math::Vector3 x{ halfExtents.x, 0.0f, 0.0f };
    const math::Vector3 y{ 0.0f, halfExtents.y, 0.0f };
    const math::Vector3 z{ 0.0f, 0.0f, halfExtents.z };

    /// @note +Z
    AddQuad(center + z,  x, y);
    /// @note -Z
    AddQuad(center - z, -x, y);
    /// @note +X
    AddQuad(center + x, -z, y);
    /// @note -X
    AddQuad(center - x,  z, y);
    /// @note +Y
    AddQuad(center + y,  x, -z);
    /// @note -Y
    AddQuad(center - y,  x,  z);
}

void MeshBuilder::AddSphere(const math::Vector3& center, float radius, int segments, int rings)
{
    const int seg  = (std::max)(3, segments);
    const int ring = (std::max)(2, rings);
    const uint32_t base = static_cast<uint32_t>(m_vertices.size());
    const int stride = seg + 1;

    /// @note s = 経度 θ, t = 緯度 φ。Cross(∂p/∂θ, ∂p/∂φ) が外向きになる並び。
    for (int j = 0; j <= ring; ++j) {
        const float v   = static_cast<float>(j) / static_cast<float>(ring);
        const float phi = v * kPi;
        const float sinPhi = std::sin(phi);
        const float cosPhi = std::cos(phi);
        for (int i = 0; i <= seg; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(seg);
            const float theta = u * kTwoPi;
            const math::Vector3 normal{ sinPhi * std::cos(theta), cosPhi, sinPhi * std::sin(theta) };
            const uint32_t index = Push(center + normal * radius, normal, { u, v });
            m_vertices[index].tangent =
                math::Vector3{ -std::sin(theta), 0.0f, std::cos(theta) }.NormalizedOr(math::Vector3::RIGHT);
        }
    }

    for (int j = 0; j < ring; ++j) {
        for (int i = 0; i < seg; ++i) {
            const uint32_t s0t0 = base + static_cast<uint32_t>(j * stride + i);
            const uint32_t s1t0 = s0t0 + 1;
            const uint32_t s0t1 = s0t0 + static_cast<uint32_t>(stride);
            const uint32_t s1t1 = s0t1 + 1;
            AddQuad(s0t1, s1t1, s1t0, s0t0);
        }
    }
}

void MeshBuilder::AddIcoSphere(const math::Vector3& center, float radius, int subdivisions)
{
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    const math::Vector3 corners[12] = {
        {-1,  t,  0}, { 1,  t,  0}, {-1, -t,  0}, { 1, -t,  0},
        { 0, -1,  t}, { 0,  1,  t}, { 0, -1, -t}, { 0,  1, -t},
        { t,  0, -1}, { t,  0,  1}, {-t,  0, -1}, {-t,  0,  1}
    };
    const uint32_t faces[20][3] = {
        {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
        {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
        {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
        {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}
    };

    MeshBuilder shell;
    shell.SetColor(m_color);
    for (const math::Vector3& corner : corners) {
        const math::Vector3 normal = corner.NormalizedOr(math::Vector3::UP);
        shell.Push(normal, normal, { 0.0f, 0.0f });
    }
    /// @note 正二十面体の面リストは «反時計回りが表» で書かれた資料が多く、そのまま使うと内側だけが
    ///       描かれる。凸多面体なので面法線が中心から外を向くかで機械的に判定して直す。
    for (const auto& face : faces) {
        const math::Vector3& a = shell.m_vertices[face[0]].position;
        const math::Vector3& b = shell.m_vertices[face[1]].position;
        const math::Vector3& c = shell.m_vertices[face[2]].position;
        const bool outward = math::Vector3::Dot(FaceNormal(a, b, c), a + b + c) > 0.0f;
        if (outward) shell.AddTriangle(face[0], face[1], face[2]);
        else         shell.AddTriangle(face[0], face[2], face[1]);
    }

    shell.Subdivide((std::max)(0, subdivisions));

    /// @note 分割で生まれた中点は球面から凹んでいるので、押し戻してから UV を張り直す。
    for (MeshVertex& vertex : shell.m_vertices) {
        const math::Vector3 normal = vertex.position.NormalizedOr(math::Vector3::UP);
        vertex.position = center + normal * radius;
        vertex.normal   = normal;
        vertex.uv       = { std::atan2(normal.z, normal.x) / kTwoPi + 0.5f,
                            std::acos((std::clamp)(normal.y, -1.0f, 1.0f)) / kPi };
    }
    shell.RecalculateTangents();
    Append(shell);
}

void MeshBuilder::AddCylinder(const math::Vector3& baseCenter, const math::Vector3& axis,
                              float radius, float height, int segments, bool capped)
{
    const int seg = (std::max)(3, segments);
    math::Vector3 a, right, up;
    AxisBasis(axis, a, right, up);

    const uint32_t base = static_cast<uint32_t>(m_vertices.size());
    const int stride = seg + 1;
    for (int j = 0; j <= 1; ++j) {
        const float v = static_cast<float>(j);
        for (int i = 0; i <= seg; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(seg);
            const float theta = u * kTwoPi;
            const math::Vector3 normal = right * std::cos(theta) + up * std::sin(theta);
            const math::Vector3 position = baseCenter + normal * radius + a * (height * v);
            const uint32_t index = Push(position, normal, { u, 1.0f - v });
            m_vertices[index].tangent =
                (up * std::cos(theta) - right * std::sin(theta)).NormalizedOr(right);
        }
    }
    for (int i = 0; i < seg; ++i) {
        const uint32_t s0t0 = base + static_cast<uint32_t>(i);
        const uint32_t s1t0 = s0t0 + 1;
        const uint32_t s0t1 = s0t0 + static_cast<uint32_t>(stride);
        const uint32_t s1t1 = s0t1 + 1;
        AddQuad(s0t1, s1t1, s1t0, s0t0);
    }

    if (capped) {
        AddDisc(baseCenter + a * height, a, radius, seg);
        AddDisc(baseCenter, -a, radius, seg);
    }
}

void MeshBuilder::AddCone(const math::Vector3& baseCenter, const math::Vector3& axis,
                          float radius, float height, int segments, bool capped)
{
    const int seg = (std::max)(3, segments);
    math::Vector3 a, right, up;
    AxisBasis(axis, a, right, up);

    /// @note 側面の法線は «半径方向 * height + 軸方向 * radius» の向き。
    const uint32_t base = static_cast<uint32_t>(m_vertices.size());
    const int stride = seg + 1;
    for (int j = 0; j <= 1; ++j) {
        const float v = static_cast<float>(j);
        for (int i = 0; i <= seg; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(seg);
            const float theta = u * kTwoPi;
            const math::Vector3 radial = right * std::cos(theta) + up * std::sin(theta);
            const math::Vector3 normal = (radial * height + a * radius).NormalizedOr(radial);
            const math::Vector3 position =
                baseCenter + radial * (radius * (1.0f - v)) + a * (height * v);
            const uint32_t index = Push(position, normal, { u, 1.0f - v });
            m_vertices[index].tangent =
                (up * std::cos(theta) - right * std::sin(theta)).NormalizedOr(right);
        }
    }
    for (int i = 0; i < seg; ++i) {
        const uint32_t s0t0 = base + static_cast<uint32_t>(i);
        const uint32_t s1t0 = s0t0 + 1;
        const uint32_t s0t1 = s0t0 + static_cast<uint32_t>(stride);
        const uint32_t s1t1 = s0t1 + 1;
        AddQuad(s0t1, s1t1, s1t0, s0t0);
    }

    if (capped) AddDisc(baseCenter, -a, radius, seg);
}

void MeshBuilder::AddCapsule(const math::Vector3& center, const math::Vector3& axis,
                             float radius, float height, int segments, int rings)
{
    const int seg  = (std::max)(3, segments);
    /// @note 半球を上下に割るので偶数段でないと赤道が作れない。
    const int ring = (std::max)(2, rings + (rings % 2));
    math::Vector3 a, right, up;
    AxisBasis(axis, a, right, up);
    /// @note AxisBasis は «+axis 方向へ進む» 曲面 (円柱・トーラス・回転体) 向けに
    ///       Cross(up, axis) == right を返すが、球と同じ緯度 φ は +axis から «離れる» 向きに
    ///       進むため外積の符号が 1 回裏返る。ここだけ基底を鏡にして AddSphere と同じ並びに揃える。
    up = -up;

    const float half = height * 0.5f;
    const uint32_t base = static_cast<uint32_t>(m_vertices.size());
    const int stride = seg + 1;
    /// @note 緯度は 0 (上の極) → ring+1 (下の極)。赤道 (φ = π/2) だけ «上半球の縁» と «下半球の縁» の
    ///       2 段を置き、その間が円柱の側面になる。継ぎ目を別扱いしないので法線がそのまま繋がる。
    const int rows = ring + 1;
    for (int j = 0; j <= rows; ++j) {
        const bool  upperHalf = j <= ring / 2;
        const int   latitude  = upperHalf ? j : j - 1;
        const float phi    = static_cast<float>(latitude) / static_cast<float>(ring) * kPi;
        const float sinPhi = std::sin(phi);
        const float cosPhi = std::cos(phi);
        const math::Vector3 hub = center + a * (upperHalf ? half : -half);
        for (int i = 0; i <= seg; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(seg);
            const float theta = u * kTwoPi;
            const math::Vector3 radial = right * std::cos(theta) + up * std::sin(theta);
            const math::Vector3 normal = radial * sinPhi + a * cosPhi;
            const uint32_t index = Push(hub + normal * radius, normal,
                                        { u, static_cast<float>(j) / static_cast<float>(rows) });
            m_vertices[index].tangent =
                (up * std::cos(theta) - right * std::sin(theta)).NormalizedOr(right);
        }
    }

    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < seg; ++i) {
            const uint32_t s0t0 = base + static_cast<uint32_t>(j * stride + i);
            const uint32_t s1t0 = s0t0 + 1;
            const uint32_t s0t1 = s0t0 + static_cast<uint32_t>(stride);
            const uint32_t s1t1 = s0t1 + 1;
            AddQuad(s0t1, s1t1, s1t0, s0t0);
        }
    }
}

void MeshBuilder::AddTorus(const math::Vector3& center, const math::Vector3& axis,
                           float majorRadius, float minorRadius,
                           int majorSegments, int minorSegments)
{
    const int majorSeg = (std::max)(3, majorSegments);
    const int minorSeg = (std::max)(3, minorSegments);
    math::Vector3 a, right, up;
    AxisBasis(axis, a, right, up);

    const uint32_t base = static_cast<uint32_t>(m_vertices.size());
    const int stride = majorSeg + 1;
    for (int j = 0; j <= minorSeg; ++j) {
        const float v   = static_cast<float>(j) / static_cast<float>(minorSeg);
        const float phi = v * kTwoPi;
        for (int i = 0; i <= majorSeg; ++i) {
            const float u     = static_cast<float>(i) / static_cast<float>(majorSeg);
            const float theta = u * kTwoPi;
            const math::Vector3 radial = right * std::cos(theta) + up * std::sin(theta);
            const math::Vector3 normal = radial * std::cos(phi) + a * std::sin(phi);
            const math::Vector3 position =
                center + radial * majorRadius + normal * minorRadius;
            const uint32_t index = Push(position, normal, { u, v });
            m_vertices[index].tangent =
                (up * std::cos(theta) - right * std::sin(theta)).NormalizedOr(right);
        }
    }

    for (int j = 0; j < minorSeg; ++j) {
        for (int i = 0; i < majorSeg; ++i) {
            const uint32_t s0t0 = base + static_cast<uint32_t>(j * stride + i);
            const uint32_t s1t0 = s0t0 + 1;
            const uint32_t s0t1 = s0t0 + static_cast<uint32_t>(stride);
            const uint32_t s1t1 = s0t1 + 1;
            AddQuad(s0t1, s1t1, s1t0, s0t0);
        }
    }
}

void MeshBuilder::AddDisc(const math::Vector3& center, const math::Vector3& normal,
                          float radius, int segments, float innerRadius)
{
    const int seg = (std::max)(3, segments);
    math::Vector3 n, right, up;
    AxisBasis(normal, n, right, up);
    const float inner = (std::clamp)(innerRadius, 0.0f, radius);

    /// @note s = 半径, t = 角度 の順にすると Cross(∂p/∂s, ∂p/∂t) が n を向く。
    const uint32_t base = static_cast<uint32_t>(m_vertices.size());
    for (int j = 0; j <= seg; ++j) {
        const float t     = static_cast<float>(j) / static_cast<float>(seg);
        const float theta = t * kTwoPi;
        const float cosTheta = std::cos(theta);
        const float sinTheta = std::sin(theta);
        const math::Vector3 radial = right * cosTheta + up * sinTheta;
        for (int i = 0; i <= 1; ++i) {
            const float rho   = i == 0 ? inner : radius;
            const float scale = radius > 0.0f ? rho / radius : 0.0f;
            const uint32_t index = Push(center + radial * rho, n,
                                        { 0.5f + cosTheta * scale * 0.5f,
                                          0.5f + sinTheta * scale * 0.5f });
            m_vertices[index].tangent = right;
        }
    }

    for (int j = 0; j < seg; ++j) {
        const uint32_t t0Inner = base + static_cast<uint32_t>(j * 2);
        const uint32_t t0Outer = t0Inner + 1;
        const uint32_t t1Inner = t0Inner + 2;
        const uint32_t t1Outer = t0Inner + 3;
        if (inner <= 0.0f) {
            /// @note 内側は 1 点に潰れるので三角形 1 枚で足りる。
            AddTriangle(t1Inner, t1Outer, t0Outer);
        } else {
            AddQuad(t1Inner, t1Outer, t0Outer, t0Inner);
        }
    }
}

void MeshBuilder::AddPolygon(std::span<const math::Vector3> points, const math::Vector3& normal)
{
    if (points.size() < 3) return;
    const math::Vector3 n = normal.NormalizedOr(math::Vector3::UP);
    math::Vector3 axis, right, up;
    AxisBasis(n, axis, right, up);

    math::Vector3 centroid = math::Vector3::ZERO;
    for (const math::Vector3& point : points) centroid += point;
    centroid = centroid / static_cast<float>(points.size());

    const uint32_t base = static_cast<uint32_t>(m_vertices.size());
    for (const math::Vector3& point : points) {
        /// @note UV は重心を原点にした平面座標。絶対座標のままだと桁が大きくなりタイリングが破綻する。
        const math::Vector3 local = point - centroid;
        const uint32_t index = Push(point, n,
            { math::Vector3::Dot(local, right), math::Vector3::Dot(local, up) });
        m_vertices[index].tangent = right;
    }
    const uint32_t count = static_cast<uint32_t>(points.size());
    for (uint32_t i = 1; i + 1 < count; ++i)
        AddTriangle(base, base + i, base + i + 1);
}

void MeshBuilder::ExtrudePolygon(std::span<const math::Vector3> points,
                                 const math::Vector3& extrusion, bool capped)
{
    if (points.size() < 3) return;
    const math::Vector3 up = extrusion.NormalizedOr(math::Vector3::UP);

    /// @note 側面。稜線 a→b の外向き法線は Cross(extrusion, b - a) 側になる。
    for (std::size_t i = 0; i < points.size(); ++i) {
        const math::Vector3& a = points[i];
        const math::Vector3& b = points[(i + 1) % points.size()];
        const math::Vector3 normal =
            math::Vector3::Cross(up, b - a).NormalizedOr(math::Vector3::FORWARD);
        const float u0 = static_cast<float>(i) / static_cast<float>(points.size());
        const float u1 = static_cast<float>(i + 1) / static_cast<float>(points.size());

        const uint32_t v0 = Push(a, normal, { u0, 1.0f });
        const uint32_t v1 = Push(b, normal, { u1, 1.0f });
        const uint32_t v2 = Push(b + extrusion, normal, { u1, 0.0f });
        const uint32_t v3 = Push(a + extrusion, normal, { u0, 0.0f });
        const math::Vector3 tangent = (b - a).NormalizedOr(math::Vector3::RIGHT);
        for (uint32_t v = v0; v <= v3; ++v) m_vertices[v].tangent = tangent;
        AddQuad(v0, v1, v2, v3);
    }

    if (!capped) return;

    std::vector<math::Vector3> cap(points.begin(), points.end());
    for (math::Vector3& point : cap) point += extrusion;
    AddPolygon(cap, up);

    /// @note 底面は法線が逆なので、周回も逆にしないと «時計回りが表» を満たさない。
    std::vector<math::Vector3> bottom(points.rbegin(), points.rend());
    AddPolygon(bottom, -up);
}

void MeshBuilder::AddLathe(std::span<const math::Vector2> profile,
                           const math::Vector3& center, const math::Vector3& axis,
                           int segments)
{
    if (profile.size() < 2) return;
    const int seg = (std::max)(3, segments);
    math::Vector3 a, right, up;
    AxisBasis(axis, a, right, up);

    const uint32_t base = static_cast<uint32_t>(m_vertices.size());
    const int stride = seg + 1;
    for (std::size_t k = 0; k < profile.size(); ++k) {
        /// @note 断面の傾きから法線を出す。両端は隣を借りる。
        const std::size_t prev = k == 0 ? 0 : k - 1;
        const std::size_t next = k + 1 < profile.size() ? k + 1 : k;
        const math::Vector2 slope{ profile[next].x - profile[prev].x,
                                   profile[next].y - profile[prev].y };
        const float v = static_cast<float>(k) / static_cast<float>(profile.size() - 1);
        for (int i = 0; i <= seg; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(seg);
            const float theta = u * kTwoPi;
            const math::Vector3 radial = right * std::cos(theta) + up * std::sin(theta);
            const math::Vector3 normal =
                (radial * slope.y - a * slope.x).NormalizedOr(radial);
            const math::Vector3 position =
                center + radial * profile[k].x + a * profile[k].y;
            const uint32_t index = Push(position, normal, { u, v });
            m_vertices[index].tangent =
                (up * std::cos(theta) - right * std::sin(theta)).NormalizedOr(right);
        }
    }

    for (std::size_t k = 0; k + 1 < profile.size(); ++k) {
        for (int i = 0; i < seg; ++i) {
            const uint32_t s0t0 = base + static_cast<uint32_t>(k * stride + i);
            const uint32_t s1t0 = s0t0 + 1;
            const uint32_t s0t1 = s0t0 + static_cast<uint32_t>(stride);
            const uint32_t s1t1 = s0t1 + 1;
            AddQuad(s0t1, s1t1, s1t0, s0t0);
        }
    }
}

void MeshBuilder::AddRibbon(std::span<const math::Vector3> points,
                            std::span<const float> widths,
                            const math::Vector3& up)
{
    if (points.size() < 2) return;
    const uint32_t base = static_cast<uint32_t>(m_vertices.size());
    const math::Vector3 facing = up.NormalizedOr(math::Vector3::UP);

    for (std::size_t i = 0; i < points.size(); ++i) {
        const math::Vector3 direction = i + 1 < points.size()
            ? points[i + 1] - points[i]
            : points[i] - points[i - 1];
        const math::Vector3 tangent = direction.NormalizedOr(math::Vector3::RIGHT);
        /// @note side = Cross(up, direction) にすると Cross(direction, side) == up になり、
        ///       帯の表が up 側を向く。
        const math::Vector3 side =
            math::Vector3::Cross(facing, tangent).NormalizedOr(math::Vector3::RIGHT);
        const float half = SampleWidth(widths, i, 1.0f) * 0.5f;
        const float v = static_cast<float>(i) / static_cast<float>(points.size() - 1);

        const uint32_t inner = Push(points[i] - side * half, facing, { v, 0.0f });
        const uint32_t outer = Push(points[i] + side * half, facing, { v, 1.0f });
        m_vertices[inner].tangent = tangent;
        m_vertices[outer].tangent = tangent;
    }

    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const uint32_t s0t0 = base + static_cast<uint32_t>(i * 2);
        const uint32_t s0t1 = s0t0 + 1;
        const uint32_t s1t0 = s0t0 + 2;
        const uint32_t s1t1 = s0t0 + 3;
        AddQuad(s0t1, s1t1, s1t0, s0t0);
    }
}

void MeshBuilder::AddRibbonFacing(std::span<const math::Vector3> points,
                                  std::span<const float> widths,
                                  const math::Vector3& viewPosition)
{
    if (points.size() < 2) return;
    const uint32_t base = static_cast<uint32_t>(m_vertices.size());

    for (std::size_t i = 0; i < points.size(); ++i) {
        const math::Vector3 direction = i + 1 < points.size()
            ? points[i + 1] - points[i]
            : points[i] - points[i - 1];
        const math::Vector3 tangent = direction.NormalizedOr(math::Vector3::RIGHT);
        const math::Vector3 toView =
            (viewPosition - points[i]).NormalizedOr(math::Vector3::UP);
        const math::Vector3 side =
            math::Vector3::Cross(toView, tangent).NormalizedOr(math::Vector3::RIGHT);
        const math::Vector3 normal = math::Vector3::Cross(tangent, side).NormalizedOr(toView);
        const float half = SampleWidth(widths, i, 1.0f) * 0.5f;
        const float v = static_cast<float>(i) / static_cast<float>(points.size() - 1);

        const uint32_t inner = Push(points[i] - side * half, normal, { v, 0.0f });
        const uint32_t outer = Push(points[i] + side * half, normal, { v, 1.0f });
        m_vertices[inner].tangent = tangent;
        m_vertices[outer].tangent = tangent;
    }

    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const uint32_t s0t0 = base + static_cast<uint32_t>(i * 2);
        const uint32_t s0t1 = s0t0 + 1;
        const uint32_t s1t0 = s0t0 + 2;
        const uint32_t s1t1 = s0t0 + 3;
        AddQuad(s0t1, s1t1, s1t0, s0t0);
    }
}

void MeshBuilder::AddTube(std::span<const math::Vector3> points,
                          std::span<const float> radii,
                          int sides, bool capped)
{
    if (points.size() < 2) return;
    const int seg = (std::max)(3, sides);
    const uint32_t base = static_cast<uint32_t>(m_vertices.size());
    const int stride = seg + 1;

    for (std::size_t k = 0; k < points.size(); ++k) {
        const math::Vector3 direction = k + 1 < points.size()
            ? points[k + 1] - points[k]
            : points[k] - points[k - 1];
        math::Vector3 a, right, up;
        AxisBasis(direction, a, right, up);
        const float radius = SampleWidth(radii, k, 0.1f);
        const float v = static_cast<float>(k) / static_cast<float>(points.size() - 1);

        for (int i = 0; i <= seg; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(seg);
            const float theta = u * kTwoPi;
            const math::Vector3 normal = right * std::cos(theta) + up * std::sin(theta);
            const uint32_t index = Push(points[k] + normal * radius, normal, { u, v });
            m_vertices[index].tangent =
                (up * std::cos(theta) - right * std::sin(theta)).NormalizedOr(right);
        }
    }

    for (std::size_t k = 0; k + 1 < points.size(); ++k) {
        for (int i = 0; i < seg; ++i) {
            const uint32_t s0t0 = base + static_cast<uint32_t>(k * stride + i);
            const uint32_t s1t0 = s0t0 + 1;
            const uint32_t s0t1 = s0t0 + static_cast<uint32_t>(stride);
            const uint32_t s1t1 = s0t1 + 1;
            AddQuad(s0t1, s1t1, s1t0, s0t0);
        }
    }

    if (capped) {
        const math::Vector3 headDir = (points[1] - points[0]).NormalizedOr(math::Vector3::UP);
        const math::Vector3 tailDir =
            (points[points.size() - 1] - points[points.size() - 2]).NormalizedOr(math::Vector3::UP);
        AddDisc(points.front(), -headDir, SampleWidth(radii, 0, 0.1f), seg);
        AddDisc(points.back(), tailDir, SampleWidth(radii, points.size() - 1, 0.1f), seg);
    }
}

/// @name 加工

void MeshBuilder::Transform(const math::Matrix4& matrix)
{
    TransformRange(0, static_cast<uint32_t>(m_vertices.size()), matrix);
}

void MeshBuilder::TransformRange(uint32_t firstVertex, uint32_t count, const math::Matrix4& matrix)
{
    const math::Matrix4 normalMatrix = math::Matrix4::InverseTransposeAffine(matrix);
    const uint32_t last = (std::min)(static_cast<uint32_t>(m_vertices.size()), firstVertex + count);
    for (uint32_t i = firstVertex; i < last; ++i) {
        MeshVertex& vertex = m_vertices[i];
        vertex.position = TransformPoint(matrix, vertex.position);
        vertex.normal   = TransformDirection(normalMatrix, vertex.normal)
                              .NormalizedOr(math::Vector3::UP);
        vertex.tangent  = TransformDirection(matrix, vertex.tangent)
                              .NormalizedOr(math::Vector3::RIGHT);
    }
}

void MeshBuilder::Append(const MeshBuilder& other)
{
    const uint32_t offset = static_cast<uint32_t>(m_vertices.size());
    m_vertices.insert(m_vertices.end(), other.m_vertices.begin(), other.m_vertices.end());
    m_indices.reserve(m_indices.size() + other.m_indices.size());
    for (uint32_t index : other.m_indices) m_indices.push_back(index + offset);
}

void MeshBuilder::Append(const MeshBuilder& other, const math::Matrix4& matrix)
{
    const uint32_t offset = static_cast<uint32_t>(m_vertices.size());
    Append(other);
    TransformRange(offset, static_cast<uint32_t>(other.m_vertices.size()), matrix);
}

void MeshBuilder::RecalculateNormals()
{
    for (MeshVertex& vertex : m_vertices) vertex.normal = math::Vector3::ZERO;

    /// @note 正規化しない面法線をそのまま足すと、面積が大きい面ほど強く効く (面積重み付き)。
    for (std::size_t i = 0; i + 2 < m_indices.size(); i += 3) {
        const uint32_t ia = m_indices[i];
        const uint32_t ib = m_indices[i + 1];
        const uint32_t ic = m_indices[i + 2];
        if (ia >= m_vertices.size() || ib >= m_vertices.size() || ic >= m_vertices.size()) continue;
        const math::Vector3 normal = FaceNormal(m_vertices[ia].position,
                                                m_vertices[ib].position,
                                                m_vertices[ic].position);
        m_vertices[ia].normal += normal;
        m_vertices[ib].normal += normal;
        m_vertices[ic].normal += normal;
    }

    for (MeshVertex& vertex : m_vertices)
        vertex.normal = vertex.normal.NormalizedOr(math::Vector3::UP);
}

void MeshBuilder::RecalculateTangents()
{
    std::vector<math::Vector3> accumulated(m_vertices.size(), math::Vector3::ZERO);

    for (std::size_t i = 0; i + 2 < m_indices.size(); i += 3) {
        const uint32_t ia = m_indices[i];
        const uint32_t ib = m_indices[i + 1];
        const uint32_t ic = m_indices[i + 2];
        if (ia >= m_vertices.size() || ib >= m_vertices.size() || ic >= m_vertices.size()) continue;

        const math::Vector3 edge1 = m_vertices[ib].position - m_vertices[ia].position;
        const math::Vector3 edge2 = m_vertices[ic].position - m_vertices[ia].position;
        const float du1 = m_vertices[ib].uv.x - m_vertices[ia].uv.x;
        const float dv1 = m_vertices[ib].uv.y - m_vertices[ia].uv.y;
        const float du2 = m_vertices[ic].uv.x - m_vertices[ia].uv.x;
        const float dv2 = m_vertices[ic].uv.y - m_vertices[ia].uv.y;

        /// @note UV が潰れている面は接線を決められない。寄与を捨てて隣の面に任せる。
        const float determinant = du1 * dv2 - du2 * dv1;
        if (std::fabs(determinant) < 1e-12f) continue;
        const float inverse = 1.0f / determinant;
        const math::Vector3 tangent = (edge1 * dv2 - edge2 * dv1) * inverse;

        accumulated[ia] += tangent;
        accumulated[ib] += tangent;
        accumulated[ic] += tangent;
    }

    for (std::size_t i = 0; i < m_vertices.size(); ++i) {
        const math::Vector3& normal = m_vertices[i].normal;
        /// @note Gram-Schmidt で法線に直交させる。どの面からも寄与が無かった頂点は既定軸へ逃がす。
        const math::Vector3 projected =
            accumulated[i] - normal * math::Vector3::Dot(normal, accumulated[i]);
        m_vertices[i].tangent = projected.NormalizedOr(math::Vector3::RIGHT);
    }
}

void MeshBuilder::FlipFaces()
{
    for (std::size_t i = 0; i + 2 < m_indices.size(); i += 3)
        std::swap(m_indices[i + 1], m_indices[i + 2]);
    FlipNormals();
}

void MeshBuilder::FlipNormals()
{
    for (MeshVertex& vertex : m_vertices) vertex.normal = -vertex.normal;
}

void MeshBuilder::Weld(float epsilon)
{
    if (m_vertices.empty()) return;
    const float cell = (std::max)(epsilon, 1e-6f);

    struct Key {
        int64_t x, y, z;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept
        {
            return std::hash<int64_t>{}(key.x)
                 ^ (std::hash<int64_t>{}(key.y) << 1)
                 ^ (std::hash<int64_t>{}(key.z) << 2);
        }
    };

    std::unordered_map<Key, uint32_t, KeyHash> unique;
    std::vector<MeshVertex> merged;
    std::vector<uint32_t>   remap(m_vertices.size());
    merged.reserve(m_vertices.size());

    for (std::size_t i = 0; i < m_vertices.size(); ++i) {
        const math::Vector3& position = m_vertices[i].position;
        const Key key{ static_cast<int64_t>(std::llround(position.x / cell)),
                       static_cast<int64_t>(std::llround(position.y / cell)),
                       static_cast<int64_t>(std::llround(position.z / cell)) };
        const auto found = unique.find(key);
        if (found != unique.end()) {
            remap[i] = found->second;
            continue;
        }
        const uint32_t index = static_cast<uint32_t>(merged.size());
        unique.emplace(key, index);
        merged.push_back(m_vertices[i]);
        remap[i] = index;
    }

    for (uint32_t& index : m_indices)
        if (index < remap.size()) index = remap[index];
    m_vertices = std::move(merged);
}

void MeshBuilder::Subdivide(int levels)
{
    for (int level = 0; level < levels; ++level) {
        if (m_indices.empty()) return;

        /// @note 稜線ごとに中点を 1 個だけ作る。作り直すと隣の面と頂点が割れて隙間になる。
        std::unordered_map<uint64_t, uint32_t> midpoints;
        const auto midpoint = [&](uint32_t a, uint32_t b) {
            const uint64_t key = a < b
                ? (static_cast<uint64_t>(a) << 32) | b
                : (static_cast<uint64_t>(b) << 32) | a;
            const auto found = midpoints.find(key);
            if (found != midpoints.end()) return found->second;

            const MeshVertex& va = m_vertices[a];
            const MeshVertex& vb = m_vertices[b];
            MeshVertex mid;
            mid.position = (va.position + vb.position) * 0.5f;
            mid.normal   = ((va.normal + vb.normal) * 0.5f).NormalizedOr(va.normal);
            mid.tangent  = ((va.tangent + vb.tangent) * 0.5f).NormalizedOr(va.tangent);
            mid.uv       = { (va.uv.x + vb.uv.x) * 0.5f, (va.uv.y + vb.uv.y) * 0.5f };
            mid.color    = { (va.color.x + vb.color.x) * 0.5f, (va.color.y + vb.color.y) * 0.5f,
                             (va.color.z + vb.color.z) * 0.5f, (va.color.w + vb.color.w) * 0.5f };

            const uint32_t index = static_cast<uint32_t>(m_vertices.size());
            m_vertices.push_back(mid);
            midpoints.emplace(key, index);
            return index;
        };

        std::vector<uint32_t> next;
        next.reserve(m_indices.size() * 4);
        for (std::size_t i = 0; i + 2 < m_indices.size(); i += 3) {
            const uint32_t a = m_indices[i];
            const uint32_t b = m_indices[i + 1];
            const uint32_t c = m_indices[i + 2];
            const uint32_t ab = midpoint(a, b);
            const uint32_t bc = midpoint(b, c);
            const uint32_t ca = midpoint(c, a);
            /// @note 4 枚とも元の三角形と同じ巻き順で並べる (裏返さない)。
            const uint32_t subTriangles[4][3] = {
                { a, ab, ca }, { ab, b, bc }, { ca, bc, c }, { ab, bc, ca }
            };
            for (const auto& tri : subTriangles) {
                next.push_back(tri[0]);
                next.push_back(tri[1]);
                next.push_back(tri[2]);
            }
        }
        m_indices = std::move(next);
    }
}

} // namespace fbzz::scene
