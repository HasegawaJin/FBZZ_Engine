/// @file    OcclusionCuller.cpp
/// @brief   内接球の確定被覆だけを使う CPU 遮蔽判定。
/// @author  Hasegawa Jin
/// @date    2026-05-31
#include <Graphics/Pipeline/OcclusionCuller.hpp>
#include <Math/Simd.hpp>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace fbzz::renderer {
namespace {

struct ProjectedPoint { double x, y, z; };
struct ProjectedBounds {
    double minX, minY, maxX, maxY, nearest;
};

bool ProjectPoint(const math::Vector3& vertex, const math::Matrix4& view, const math::Matrix4& projection,
    float nearZ, ProjectedPoint& point)
{
    const auto position = view * math::Vector4{vertex, 1};
    const auto clip = projection * position;
    if (!std::isfinite(position.z) || position.z <= nearZ || !std::isfinite(clip.w) || clip.w <= 0) return false;
    /// @note Low Y and high X retain the scalar division, shift and two multiplies in the same order.
    const auto ratios = _mm_div_pd(_mm_set_pd(clip.x, clip.y), _mm_set1_pd(clip.w));
    const auto shifted = _mm_addsub_pd(_mm_set1_pd(1.0), ratios);
    const auto pixels = _mm_mul_pd(_mm_mul_pd(shifted, _mm_set1_pd(0.5)),
        _mm_set_pd(OcclusionCuller::kWidth, OcclusionCuller::kHeight));
    point = {_mm_cvtsd_f64(_mm_unpackhi_pd(pixels, pixels)), _mm_cvtsd_f64(pixels), position.z};
    return std::isfinite(point.x) && std::isfinite(point.y);
}

/// @note 4 頂点をレーンへ並べ、各成分の積・加算順は Matrix4::operator* と揃える。
/// @see Docs/design/math-simd.md SSE4.1 と非 FMA の演算順。
unsigned ProjectBoxPlane(const math::Vector3& low, const math::Vector3& high, float planeZ,
    const math::Matrix4& view, const math::Matrix4& projection, float nearZ, ProjectedPoint* points)
{
    using math::simd::Vec;
    const auto transform = [](const float* row, Vec x, Vec y, Vec z, Vec w) {
        auto result = _mm_mul_ps(_mm_set1_ps(row[0]), x);
        result = math::simd::MulAdd(_mm_set1_ps(row[1]), y, result);
        result = math::simd::MulAdd(_mm_set1_ps(row[2]), z, result);
        return math::simd::MulAdd(_mm_set1_ps(row[3]), w, result);
    };
    const auto x = _mm_setr_ps(low.x, high.x, high.x, low.x);
    const auto y = _mm_setr_ps(low.y, low.y, high.y, high.y);
    const auto z4 = _mm_set1_ps(planeZ);
    const auto one = _mm_set1_ps(1.0f);
    const auto positionX = transform(view.m[0], x, y, z4, one);
    const auto positionY = transform(view.m[1], x, y, z4, one);
    const auto positionZ = transform(view.m[2], x, y, z4, one);
    const auto positionW = transform(view.m[3], x, y, z4, one);
    auto clipX = transform(projection.m[0], positionX, positionY, positionZ, positionW);
    auto clipY = transform(projection.m[1], positionX, positionY, positionZ, positionW);
    auto clipW = transform(projection.m[3], positionX, positionY, positionZ, positionW);

    /// @note 有限 float 同士の比は double の範囲内に収まるため、X/Y は除算前に検証できる。
    const auto maximum = _mm_set1_ps((std::numeric_limits<float>::max)());
    const auto sign = _mm_set1_ps(-0.0f);
    auto valid = _mm_and_ps(_mm_cmpgt_ps(positionZ, _mm_set1_ps(nearZ)), _mm_cmple_ps(positionZ, maximum));
    valid = _mm_and_ps(valid, _mm_and_ps(_mm_cmpgt_ps(clipW, _mm_setzero_ps()), _mm_cmple_ps(clipW, maximum)));
    valid = _mm_and_ps(valid, _mm_cmple_ps(_mm_andnot_ps(sign, clipX), maximum));
    valid = _mm_and_ps(valid, _mm_cmple_ps(_mm_andnot_ps(sign, clipY), maximum));
    const auto mask = static_cast<unsigned>(_mm_movemask_ps(valid));
    if (mask == 0) return 0;
    if (mask != 0xF) {
        clipX = _mm_and_ps(valid, clipX);
        clipY = _mm_and_ps(valid, clipY);
        clipW = _mm_blendv_ps(one, clipW, valid);
    }

    double screenX[4], screenY[4];
    const auto toPixels = [](Vec clipX4, Vec clipY4, Vec clipW4, double* pixelX, double* pixelY) {
        const auto denominator = _mm_cvtps_pd(clipW4);
        const auto offset = _mm_set1_pd(1.0);
        const auto half = _mm_set1_pd(0.5);
        const auto xRatio = _mm_div_pd(_mm_cvtps_pd(clipX4), denominator);
        const auto yRatio = _mm_div_pd(_mm_cvtps_pd(clipY4), denominator);
        _mm_storeu_pd(pixelX, _mm_mul_pd(_mm_mul_pd(_mm_add_pd(offset, xRatio), half), _mm_set1_pd(OcclusionCuller::kWidth)));
        _mm_storeu_pd(pixelY, _mm_mul_pd(_mm_mul_pd(_mm_sub_pd(offset, yRatio), half), _mm_set1_pd(OcclusionCuller::kHeight)));
    };
    toPixels(clipX, clipY, clipW, screenX, screenY);
    toPixels(_mm_movehl_ps(clipX, clipX), _mm_movehl_ps(clipY, clipY), _mm_movehl_ps(clipW, clipW),
        screenX + 2, screenY + 2);
    float depth[4];
    math::simd::Store4(depth, positionZ);
    for (size_t i = 0; i < 4; ++i) points[i] = {screenX[i], screenY[i], depth[i]};
    return mask;
}

/// @note A float cell reaches the double threshold exactly when it reaches the threshold rounded up to float.
/// @note Keep subnormal thresholds in the scalar comparison so flush-to-zero cannot change the result.
/// @see Docs/design/math-simd.md SSE4.1 and unaligned loads.
bool HasVisibleDepthCell(const float* depths, int x0, int y0, int x1, int y1, double depth)
{
    constexpr int CELLS_PER_VECTOR = 4;
    if (x1 - x0 < CELLS_PER_VECTOR - 1 || !(depth >= (std::numeric_limits<float>::min)())) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                if (depths[y * OcclusionCuller::kWidth + x] >= depth) return true;
        return false;
    }

    /// @note A visible first cell needs no vector threshold setup.
    if (depths[y0 * OcclusionCuller::kWidth + x0] >= depth) return true;
    float threshold = static_cast<float>(depth);
    if (static_cast<double>(threshold) < depth)
        threshold = std::nextafter(threshold, std::numeric_limits<float>::infinity());
    const auto threshold4 = _mm_set1_ps(threshold);
    for (int y = y0; y <= y1; ++y) {
        const float* row = depths + y * OcclusionCuller::kWidth;
        int x = x0;
        for (; x + CELLS_PER_VECTOR - 1 <= x1; x += CELLS_PER_VECTOR) {
            const auto visible = _mm_cmpge_ps(math::simd::Load4(row + x), threshold4);
            if (_mm_movemask_ps(visible) != 0) return true;
        }
        for (; x <= x1; ++x)
            if (row[x] >= threshold) return true;
    }
    return false;
}

/// @note 画面内の頂点を含むセルは、外接矩形を使う深度検査の範囲にも含まれる。
float ReadCornerDepth(const ProjectedPoint& point, const float* depths)
{
    if (!(point.x >= 0 && point.x < OcclusionCuller::kWidth
        && point.y >= 0 && point.y < OcclusionCuller::kHeight)) return -1.0f;
    const int x = static_cast<int>(point.x), y = static_cast<int>(point.y);
    return depths[y * OcclusionCuller::kWidth + x];
}

double AabbDepthThreshold(double nearest, float nearZ, float farZ)
{
    return std::clamp((nearest - nearZ) / (double(farZ) - nearZ) - 1.0e-5, 0.0, 1.0);
}

/// @note 最近頂点の深度閾値は、既に投影した頂点だけで求めた閾値以下。
bool HasVisibleAabbCorner(const ProjectedPoint& point, float nearZ, float farZ, const float* depths)
{
    const float cell = ReadCornerDepth(point, depths);
    return cell == 1.0f || (cell >= 0 && cell >= AabbDepthThreshold(std::min(double(farZ), point.z), nearZ, farZ));
}

bool TestProjectedBounds(const ProjectedBounds& bounds, float nearZ, float farZ, const float* depths)
{
    constexpr double margin = 0.01;
    const double minX = bounds.minX - margin, minY = bounds.minY - margin;
    const double maxX = bounds.maxX + margin, maxY = bounds.maxY + margin;
    if (minX >= OcclusionCuller::kWidth || minY >= OcclusionCuller::kHeight || maxX < 0 || maxY < 0) return true;
    const int x0 = static_cast<int>(std::clamp(minX, 0.0, double(OcclusionCuller::kWidth - 1)));
    const int x1 = static_cast<int>(std::clamp(maxX, 0.0, double(OcclusionCuller::kWidth - 1)));
    const int y0 = static_cast<int>(std::clamp(minY, 0.0, double(OcclusionCuller::kHeight - 1)));
    const int y1 = static_cast<int>(std::clamp(maxY, 0.0, double(OcclusionCuller::kHeight - 1)));
    const double depth = AabbDepthThreshold(bounds.nearest, nearZ, farZ);
    return HasVisibleDepthCell(depths, x0, y0, x1, y1, depth);
}

} /// @note namespace

OcclusionCuller::OcclusionCuller()
{
    std::fill(std::begin(m_depth), std::end(m_depth), 1.0f);
}

void OcclusionCuller::Reset(const Camera& cam, float jitterX, float jitterY)
{
    if (++m_resetVersion == 0) ++m_resetVersion;
    m_hasDepth = false;
    m_view = cam.GetViewMatrix();
    m_proj = cam.GetProjectionMatrix();
    m_nearZ = cam.m_near;
    m_farZ = cam.m_far;
    m_orthographic = cam.m_projection == ProjectionMode::Orthographic;
    m_valid = std::isfinite(m_nearZ) && std::isfinite(m_farZ) && m_nearZ > 0 && m_farZ > m_nearZ
        && std::isfinite(jitterX) && std::isfinite(jitterY);
    for (int col = 0; col < 4; ++col) {
        m_proj.m[0][col] += jitterX * m_proj.m[3][col];
        m_proj.m[1][col] += jitterY * m_proj.m[3][col];
    }
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            m_valid = m_valid && std::isfinite(m_view.m[row][col]) && std::isfinite(m_proj.m[row][col]);
    std::fill(std::begin(m_depth), std::end(m_depth), 1.0f);
}

void OcclusionCuller::CopyGpuRows(std::span<math::Vector4> rows) const
{
    if (rows.size() < kGpuRows) return;
    for (size_t i = 0; i < 4; ++i) {
        rows[i] = {m_view.m[i][0], m_view.m[i][1], m_view.m[i][2], m_view.m[i][3]};
        rows[i + 4] = {m_proj.m[i][0], m_proj.m[i][1], m_proj.m[i][2], m_proj.m[i][3]};
    }
    rows[8] = {m_nearZ, m_farZ, float(kWidth), m_valid && m_hasDepth ? float(kHeight) : 0.0f};
    for (size_t i = 0; i < kWidth * kHeight / 4; ++i)
        rows[9 + i] = {m_depth[i * 4], m_depth[i * 4 + 1], m_depth[i * 4 + 2], m_depth[i * 4 + 3]};
}

bool OcclusionCuller::TestAabb(const math::Vector3& low, const math::Vector3& high) const
{
    if (!m_valid || !m_hasDepth || low.x > high.x || low.y > high.y || low.z > high.z) return true;
    ProjectedPoint top;
    if (!ProjectPoint({low.x,high.y,low.z}, m_view, m_proj, m_nearZ, top)
        || HasVisibleAabbCorner(top, m_nearZ, m_farZ, m_depth)) return true;
    ProjectedBounds bounds{top.x,top.y,top.x,top.y,std::min(double(m_farZ), top.z)};
    std::array<ProjectedPoint,8> points;
    if (ProjectBoxPlane(low, high, low.z, m_view, m_proj, m_nearZ, points.data()) != 0xF
        || ProjectBoxPlane(low, high, high.z, m_view, m_proj, m_nearZ, points.data() + 4) != 0xF) return true;
    constexpr std::array<size_t,7> REMAINING_CORNERS{0,1,2,4,5,7,6};
    for (size_t i : REMAINING_CORNERS) {
        const auto& point = points[i];
        bounds.minX = std::min(bounds.minX, point.x); bounds.maxX = std::max(bounds.maxX, point.x);
        bounds.minY = std::min(bounds.minY, point.y); bounds.maxY = std::max(bounds.maxY, point.y);
        bounds.nearest = std::min(bounds.nearest, point.z);
    }
    return TestProjectedBounds(bounds, m_nearZ, m_farZ, m_depth);
}

void OcclusionCuller::RasterTriangle(const math::Vector3& a, const math::Vector3& b, const math::Vector3& c)
{
    RasterPolygon(std::array{a, b, c});
}

void OcclusionCuller::RasterQuad(const math::Vector3& a, const math::Vector3& b,
    const math::Vector3& c, const math::Vector3& d)
{
    if (!RasterPolygon(std::array{a, b, c, d})) {
        RasterTriangle(a, b, c);
        RasterTriangle(a, c, d);
    }
}

namespace {

template <size_t COUNT>
bool RasterProjectedPolygon(const std::array<ProjectedPoint, COUNT>& points, float nearZ, float farZ,
    float* depths, bool& hasDepth)
{
    constexpr int kWidth = OcclusionCuller::kWidth, kHeight = OcclusionCuller::kHeight;
    constexpr size_t count = COUNT;
    double left = points[0].x, right = left, top = points[0].y, bottom = top, farthest = points[0].z;
    for (size_t i = 1; i < count; ++i) {
        left = std::min(left, points[i].x); right = std::max(right, points[i].x);
        top = std::min(top, points[i].y); bottom = std::max(bottom, points[i].y);
        farthest = std::max(farthest, points[i].z);
    }
    left = std::max(left, 0.0); right = std::min(right, double(kWidth));
    top = std::max(top, 0.0); bottom = std::min(bottom, double(kHeight));
    if (right - left < 1 || bottom - top < 1) return true;
    const int firstX = int(std::ceil(left)), lastX = int(std::floor(right)) - 1;
    const int firstY = int(std::ceil(top)), endY = int(std::floor(bottom));
    /// @note A triangle subset cannot cover a whole cell if the projected box contains none.
    if (firstX > lastX || firstY >= endY) return true;
    const auto edge = [](const ProjectedPoint& p, const ProjectedPoint& q, double x, double y) {
        return (q.x - p.x) * (y - p.y) - (q.y - p.y) * (x - p.x);
    };
    /// @note D3D の時計回り表面だけを採用し、地形の裏側から遮蔽を作らない。
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d11/conservative-rasterization Underestimated rasterization.
    for (size_t i = 0; i < count; ++i) {
        const auto& next = points[(i + 2) % count];
        if (edge(points[i], points[(i + 1) % count], next.x, next.y) <= 1.0e-8) return false;
    }
    constexpr double margin = 0.002;
    const float depth = std::nextafter(float(std::clamp(
        (farthest - nearZ) / (farZ - nearZ), 0.0, 1.0)),
        std::numeric_limits<float>::infinity());
    struct CoverageEdge { double x, y, constant, inset, deltaY, cornerX, cornerY; };
    std::array<CoverageEdge,COUNT> edges{};
    for (size_t e = 0; e < count; ++e) {
        const auto& p = points[e]; const auto& q = points[(e + 1) % count];
        const double nx = p.y - q.y, ny = q.x - p.x;
        const double inset = margin * (std::abs(nx) + std::abs(ny));
        edges[e] = {nx,ny,-nx * p.x - ny * p.y + std::min(nx,0.0) + std::min(ny,0.0) - inset,inset,
            q.y - p.y, q.y > p.y ? 1.0 : 0.0, q.x > p.x ? 0.0 : 1.0};
    }
    const auto covered = [&](int x, int y) {
        for (size_t e = 0; e < count; ++e) {
            const auto& p = points[e];
            const auto& line = edges[e];
            const double sx = x + line.cornerX, sy = y + line.cornerY;
            if (line.y * (sy - p.y) - line.deltaY * (sx - p.x) <= line.inset) return false;
        }
        return true;
    };
    /// @note Clamp と floor/ceil は単調なので、半平面の境界を集約してから一度だけセル番号に丸める。
    const double firstBoundary = firstX - 1.0, lastBoundary = lastX + 1.0;
    for (int y = firstY; y < endY; ++y) {
        double lower = firstBoundary, upper = lastBoundary;
        for (size_t e = 0; e < count && lower < upper; ++e) {
            const auto& line = edges[e];
            const double value = line.y * y + line.constant;
            if (line.x > 0) {
                lower = std::max(lower, -value / line.x + 1.0e-6);
            } else if (line.x < 0) {
                upper = std::min(upper, -value / line.x - 1.0e-6);
            } else if (value <= 0) lower = upper;
        }
        if (lower >= upper) continue;
        int x0 = int(std::floor(std::clamp(lower, -1.0, double(kWidth)))) + 1;
        int x1 = int(std::ceil(std::clamp(upper, -1.0, double(kWidth)))) - 1;
        while (x0 <= x1 && !covered(x0,y)) ++x0;
        while (x0 < x1 && !covered(x1,y)) --x1;
        for (int x = x0; x <= x1; ++x) depths[y * kWidth + x] = std::min(depths[y * kWidth + x],depth);
        hasDepth = hasDepth || x0 <= x1;
    }
    return true;
}

} /// @note namespace

bool OcclusionCuller::RasterPolygon(std::span<const math::Vector3> vertices)
{
    if (!m_valid) return false;
    std::array<ProjectedPoint,4> points{};
    for (size_t i = 0; i < vertices.size(); ++i)
        if (!ProjectPoint(vertices[i], m_view, m_proj, m_nearZ, points[i])) return false;
    if (vertices.size() == 3)
        return RasterProjectedPolygon(std::array{points[0],points[1],points[2]}, m_nearZ, m_farZ, m_depth, m_hasDepth);
    return RasterProjectedPolygon(points, m_nearZ, m_farZ, m_depth, m_hasDepth);
}

void OcclusionCuller::RasterBox(const math::Vector3& low, const math::Vector3& high, uint8_t faceMask)
{
    if (!m_valid || faceMask == 0) return;
    constexpr std::array<size_t,8> AXES{0,1,3,2,4,5,7,6};
    constexpr unsigned ALL_CORNERS = 0xFF;
    std::array<ProjectedPoint,8> points;
    unsigned tested = 0, valid = 0;
    const auto project = [&](size_t index) {
        const unsigned bit = 1u << index;
        if (!(tested & bit)) {
            tested |= bit;
            const size_t axes = AXES[index];
            if (ProjectPoint({(axes & 1) ? high.x : low.x, (axes & 2) ? high.y : low.y, (axes & 4) ? high.z : low.z},
                m_view, m_proj, m_nearZ, points[index])) valid |= bit;
        }
        return (valid & bit) != 0;
    };
    constexpr size_t TOP_CORNER = 3;
    if (m_hasDepth && low.x <= high.x && low.y <= high.y && low.z <= high.z
        && project(TOP_CORNER) && !HasVisibleAabbCorner(points[TOP_CORNER], m_nearZ, m_farZ, m_depth)) {
        const unsigned lowMask = ProjectBoxPlane(low, high, low.z, m_view, m_proj, m_nearZ, points.data());
        const unsigned highMask = ProjectBoxPlane(low, high, high.z, m_view, m_proj, m_nearZ, points.data() + 4);
        valid = lowMask | (highMask << 4);
        tested = ALL_CORNERS;
        if (valid == ALL_CORNERS) {
            const double infinity = std::numeric_limits<double>::infinity();
            ProjectedBounds bounds{infinity,infinity,-infinity,-infinity,m_farZ};
            /// @note 面と外接矩形で同じ頂点投影を共有する。
            constexpr std::array<size_t,8> CORNERS{0,1,3,2,4,5,7,6};
            for (size_t index : CORNERS) {
                const auto& point = points[index];
                bounds.minX = std::min(bounds.minX, point.x); bounds.maxX = std::max(bounds.maxX, point.x);
                bounds.minY = std::min(bounds.minY, point.y); bounds.maxY = std::max(bounds.maxY, point.y);
                bounds.nearest = std::min(bounds.nearest, point.z);
            }
            /// @note 面は投影済み 8 頂点の部分集合。箱に完全なセルがなければ、どの面も深度を書かない。
            if (std::ceil(std::max(bounds.minX, 0.0)) >= std::floor(std::min(bounds.maxX, double(kWidth)))
                || std::ceil(std::max(bounds.minY, 0.0)) >= std::floor(std::min(bounds.maxY, double(kHeight)))) return;
            if (!TestProjectedBounds(bounds, m_nearZ, m_farZ, m_depth)) return;
        }
    }
    constexpr std::array<std::array<size_t,4>,6> FACES{{
        {0,3,2,1},{4,5,6,7},{0,4,7,3},{1,2,6,5},{0,1,5,4},{3,7,6,2}}};
    for (size_t index = 0; index < FACES.size(); ++index) {
        if (!(faceMask & (1u << index))) continue;
        const auto& face = FACES[index];
        if (project(face[0]) && project(face[2])) {
            const bool first = project(face[1]), second = project(face[3]);
            if (first && second && RasterProjectedPolygon(
                std::array{points[face[0]], points[face[1]], points[face[2]], points[face[3]]},
                m_nearZ, m_farZ, m_depth, m_hasDepth)) continue;
            /// @note 近面を越えた面や非凸面は、有効な元の三角形だけを登録する。
            if (first) RasterProjectedPolygon(std::array{points[face[0]], points[face[1]], points[face[2]]},
                m_nearZ, m_farZ, m_depth, m_hasDepth);
            if (second) RasterProjectedPolygon(std::array{points[face[0]], points[face[2]], points[face[3]]},
                m_nearZ, m_farZ, m_depth, m_hasDepth);
        }
    }
}

bool OcclusionCuller::TestAndRaster(const math::Vector3& worldCenter, float worldRadius,
    float opaqueInteriorRadius)
{
    if (!m_hasDepth && !(opaqueInteriorRadius > 0)) return true;
    if (!m_valid || !std::isfinite(worldRadius) || worldRadius <= 0
        || !std::isfinite(worldCenter.x) || !std::isfinite(worldCenter.y) || !std::isfinite(worldCenter.z)) return true;
    const auto transform = [&](int row) {
        return double(m_view.m[row][0])*worldCenter.x + double(m_view.m[row][1])*worldCenter.y
            + double(m_view.m[row][2])*worldCenter.z + m_view.m[row][3];
    };
    const double cx = transform(0), cy = transform(1), cz = transform(2);
    /// @note Mesh の外接球証明で許容する float 丸めと同じ余白を検査・背面深度へ含める。
    const double radius = double(worldRadius) + (std::max)(1.0, double(worldRadius))*1.0e-5;
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(cz) || cz - radius <= m_nearZ) return true;
    const auto project = [&](double x, double y, double z, double& sx, double& sy) {
        const double w = double(m_proj.m[3][0])*x + double(m_proj.m[3][1])*y + double(m_proj.m[3][2])*z + m_proj.m[3][3];
        if (!std::isfinite(w) || w <= 0) return false;
        sx = ((double(m_proj.m[0][0])*x + double(m_proj.m[0][1])*y + double(m_proj.m[0][2])*z + m_proj.m[0][3])/w + 1)*0.5*kWidth;
        sy = (1 - (double(m_proj.m[1][0])*x + double(m_proj.m[1][1])*y + double(m_proj.m[1][2])*z + m_proj.m[1][3])/w)*0.5*kHeight;
        return std::isfinite(sx) && std::isfinite(sy);
    };
    double minX = std::numeric_limits<double>::infinity(), minY = minX;
    double maxX = -minX, maxY = -minX;
    /// @note r/z は透視投影の球を過小評価する。球を包むビュー空間の箱を全頂点で投影する。
    /// @see https://www.pbr-book.org/4ed/Shapes/Basic_Shape_Interface Bounds must enclose every surface point.
    if (m_proj.m[0][1] == 0 && m_proj.m[1][0] == 0 && m_proj.m[3][0] == 0 && m_proj.m[3][1] == 0) {
        /// @note 標準の透視・正投影では x/y が分離する。同じ箱の全頂点の極値を 2 深度で厳密に求める。
        const auto pixel = [](__m128d clip, __m128d w) {
            const auto shifted = _mm_addsub_pd(_mm_set1_pd(1.0), _mm_div_pd(clip, w));
            return _mm_mul_pd(_mm_mul_pd(shifted, _mm_set1_pd(0.5)), _mm_set_pd(kWidth, kHeight));
        };
        const auto scale = _mm_set_pd(m_proj.m[0][0], m_proj.m[1][1]);
        const auto low = _mm_set_pd(cx - radius, cy + radius);
        const auto high = _mm_set_pd(cx + radius, cy - radius);
        for (int side = 0; side < 2; ++side) {
            const double z = cz + (side ? radius : -radius);
            const double w = double(m_proj.m[3][2]) * z + m_proj.m[3][3];
            if (!std::isfinite(w) || w <= 0) return true;
            const auto offset = _mm_add_pd(_mm_mul_pd(_mm_set_pd(m_proj.m[0][2], m_proj.m[1][2]), _mm_set1_pd(z)),
                _mm_set_pd(m_proj.m[0][3], m_proj.m[1][3]));
            const auto leftTop = pixel(_mm_add_pd(_mm_mul_pd(scale, low), offset), _mm_set1_pd(w));
            const double a = _mm_cvtsd_f64(_mm_unpackhi_pd(leftTop, leftTop)), d = _mm_cvtsd_f64(leftTop);
            if (!std::isfinite(a) || !std::isfinite(d)) return true;
            if (!(opaqueInteriorRadius > 0) && ReadCornerDepth({a,d,z}, m_depth) == 1.0f) return true;
            const auto rightBottom = pixel(_mm_add_pd(_mm_mul_pd(scale, high), offset), _mm_set1_pd(w));
            const double b = _mm_cvtsd_f64(_mm_unpackhi_pd(rightBottom, rightBottom)), c = _mm_cvtsd_f64(rightBottom);
            if (!std::isfinite(b) || !std::isfinite(c)) return true;
            minX = std::min({minX,a,b}); maxX = std::max({maxX,a,b});
            minY = std::min({minY,c,d}); maxY = std::max({maxY,c,d});
        }
    } else {
        for (int corner = 0; corner < 8; ++corner) {
            double sx, sy;
            if (!project(cx + ((corner & 1) ? radius : -radius), cy + ((corner & 2) ? radius : -radius),
                cz + ((corner & 4) ? radius : -radius), sx, sy)) return true;
            minX = (std::min)(minX, sx); maxX = (std::max)(maxX, sx);
            minY = (std::min)(minY, sy); maxY = (std::max)(maxY, sy);
        }
    }
    constexpr double PIXEL_MARGIN = 0.001;
    minX -= PIXEL_MARGIN; minY -= PIXEL_MARGIN;
    maxX += PIXEL_MARGIN; maxY += PIXEL_MARGIN;
    if (minX >= kWidth || minY >= kHeight || maxX < 0 || maxY < 0) return true;
    const int x0 = static_cast<int>(std::clamp(minX, 0.0, double(kWidth - 1)));
    const int y0 = static_cast<int>(std::clamp(minY, 0.0, double(kHeight - 1)));
    const int x1 = static_cast<int>(std::clamp(maxX, 0.0, double(kWidth - 1)));
    const int y1 = static_cast<int>(std::clamp(maxY, 0.0, double(kHeight - 1)));
    const double nearDepth = std::clamp((cz - radius - m_nearZ)/(double(m_farZ) - m_nearZ), 0.0, 1.0);
    if (!HasVisibleDepthCell(m_depth, x0, y0, x1, y1, nearDepth)) return false;
    if (!std::isfinite(opaqueInteriorRadius) || opaqueInteriorRadius <= 0 || opaqueInteriorRadius > worldRadius
        || cx*cx + cy*cy + cz*cz <= radius*radius) return true;
    double sx, sy;
    if (!project(cx, cy, cz, sx, sy)) return true;
    /// @note 内接球の中心深度にある円盤は実形状内部。円盤の内接正方形でセル全体が覆われる範囲だけ焼く。
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3d11/conservative-rasterization Underestimated coverage for occluders.
    const double scale = m_orthographic ? 1 : 1/cz;
    constexpr double INSCRIBED_SCALE = 0.7071067811865475;
    const double rx = opaqueInteriorRadius*scale*std::abs(m_proj.m[0][0])*kWidth*0.5*INSCRIBED_SCALE;
    const double ry = opaqueInteriorRadius*scale*std::abs(m_proj.m[1][1])*kHeight*0.5*INSCRIBED_SCALE;
    const double left = sx - rx + PIXEL_MARGIN, right = sx + rx - PIXEL_MARGIN;
    const double top = sy - ry + PIXEL_MARGIN, bottom = sy + ry - PIXEL_MARGIN;
    if (left >= kWidth || top >= kHeight || right <= 0 || bottom <= 0) return true;
    const int ox0 = static_cast<int>(std::ceil(std::clamp(left, 0.0, double(kWidth))));
    const int oy0 = static_cast<int>(std::ceil(std::clamp(top, 0.0, double(kHeight))));
    const int ox1 = static_cast<int>(std::floor(std::clamp(right, 0.0, double(kWidth)))) - 1;
    const int oy1 = static_cast<int>(std::floor(std::clamp(bottom, 0.0, double(kHeight)))) - 1;
    /// @note 実表面の出口は外接球の背面より手前。深度は遠方へ丸め、誤った手前判定を防ぐ。
    const float farDepth = std::nextafter(static_cast<float>(std::clamp(
        (cz + radius - m_nearZ)/(double(m_farZ) - m_nearZ), 0.0, 1.0)), std::numeric_limits<float>::infinity());
    for (int y = oy0; y <= oy1; ++y)
        for (int x = ox0; x <= ox1; ++x)
            { m_depth[y*kWidth + x] = (std::min)(m_depth[y*kWidth + x], farDepth); m_hasDepth = true; }
    return true;
}

} /// @note namespace fbzz::renderer
