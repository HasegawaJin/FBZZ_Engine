/// @file    PresentationGeometry.cpp
/// @brief   Line の色付き帯・筒と Sprite の atlas 内 nine-slice を生成する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Systems/PresentationGeometry.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace fbzz::scene {
namespace {
bool Finite(const math::Vector3& value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
bool Finite(const math::Vector4& value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w); }
void Append(std::vector<float>& output, const math::Vector3& value) { output.insert(output.end(), {value.x, value.y, value.z}); }
void Append(std::vector<float>& output, const math::Vector4& value) { output.insert(output.end(), {value.x, value.y, value.z, value.w}); }
math::Vector3 Divide(const math::Vector3& a, const math::Vector3& b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
math::Vector3 CrossSide(const math::Vector3& direction, const math::Vector3& view)
{
    auto side = math::Vector3::Cross(direction, view);
    if (side.LengthSq() <= 0.000001f)
        side = math::Vector3::Cross(direction, std::abs(direction.y) < 0.9f ? math::Vector3::UP : math::Vector3::RIGHT);
    return side.NormalizedOr(math::Vector3::RIGHT);
}
void Quad(std::vector<renderer::Vertex>& vertices, std::vector<uint32_t>& indices,
    float left, float top, float right, float bottom, float u0, float v0, float u1, float v1)
{
    const auto base = static_cast<uint32_t>(vertices.size());
    vertices.insert(vertices.end(), {
        {{left, top, 0}, math::Vector3::FORWARD, math::Vector3::RIGHT, {u0, v0}},
        {{right, top, 0}, math::Vector3::FORWARD, math::Vector3::RIGHT, {u1, v0}},
        {{right, bottom, 0}, math::Vector3::FORWARD, math::Vector3::RIGHT, {u1, v1}},
        {{left, bottom, 0}, math::Vector3::FORWARD, math::Vector3::RIGHT, {u0, v1}}
    });
    indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}
void SliceAxis(float start, float end, float uvStart, float uvEnd, float sourcePixels,
    float lowBorder, float highBorder, float pixelsPerUnit, std::array<float, 4>& coordinates, std::array<float, 4>& uv)
{
    const float lowPixels = std::clamp(lowBorder, 0.0f, sourcePixels);
    const float highPixels = std::clamp(highBorder, 0.0f, sourcePixels - lowPixels);
    float low = lowPixels / pixelsPerUnit;
    float high = highPixels / pixelsPerUnit;
    const float extent = std::abs(end - start);
    if (low + high > extent && low + high > 0) {
        const float scale = extent / (low + high);
        low *= scale; high *= scale;
    }
    const float direction = end >= start ? 1.0f : -1.0f;
    coordinates = {start, start + direction * low, end - direction * high, end};
    uv = {uvStart, uvStart + (uvEnd - uvStart) * lowPixels / sourcePixels,
        uvEnd - (uvEnd - uvStart) * highPixels / sourcePixels, uvEnd};
}
} /// @note namespace

void CaptureLineGeometryInputs(const LineRendererComponent& line, const Transform& transform,
    const math::Vector3* cameraWorldPosition, std::vector<float>& output)
{
    CaptureLineGeometryInputs(LineGeometryView{line, line.points, line.space},
        transform, cameraWorldPosition, output);
}

void CaptureLineGeometryInputs(const LineGeometryView& geometry, const Transform& transform,
    const math::Vector3* cameraWorldPosition, std::vector<float>& output)
{
    const LineRendererComponent& line = geometry.settings;
    output.clear();
    output.insert(output.end(), {line.startWidth, line.endWidth, static_cast<float>(geometry.space),
        static_cast<float>(line.loop), static_cast<float>(line.billboard), static_cast<float>(line.shape),
        static_cast<float>(line.radialSegments)});
    Append(output, line.startColor); Append(output, line.endColor);
    for (const auto& point : geometry.points) Append(output, point);
    const bool cameraFacing = line.shape == LineShape::Ribbon && line.billboard && cameraWorldPosition;
    output.push_back(cameraFacing ? 1.0f : 0.0f);
    if (geometry.space == LineSpace::World || cameraFacing) {
        Append(output, transform.worldPosition); Append(output, transform.worldScale);
        output.insert(output.end(), {transform.worldRotation.x, transform.worldRotation.y, transform.worldRotation.z, transform.worldRotation.w});
    }
    if (cameraFacing) Append(output, *cameraWorldPosition);
}

void CaptureSpriteGeometryInputs(const SpriteRendererComponent& sprite, const asset::ResolvedSprite& resolved,
    std::vector<float>& output)
{
    output = {sprite.size.x, sprite.size.y, sprite.pivot.x, sprite.pivot.y, static_cast<float>(sprite.drawMode),
        static_cast<float>(sprite.flipX), static_cast<float>(sprite.flipY), resolved.uvMin.x, resolved.uvMin.y,
        resolved.uvMax.x, resolved.uvMax.y, resolved.sizePixels.x, resolved.sizePixels.y, resolved.pixelsPerUnit};
    Append(output, resolved.border);
}

bool BuildLineGeometry(const LineRendererComponent& line, const Transform& transform,
    const math::Vector3* cameraWorldPosition, std::vector<renderer::Vertex>& vertices, std::vector<uint32_t>& indices)
{
    return BuildLineGeometry(LineGeometryView{line, line.points, line.space},
        transform, cameraWorldPosition, vertices, indices);
}

bool BuildLineGeometry(const LineGeometryView& geometry, const Transform& transform,
    const math::Vector3* cameraWorldPosition, std::vector<renderer::Vertex>& vertices, std::vector<uint32_t>& indices)
{
    const LineRendererComponent& line = geometry.settings;
    const std::span<const math::Vector3> points = geometry.points;
    vertices.clear(); indices.clear();
    if (points.size() < 2 || !Finite(line.startColor) || !Finite(line.endColor)
        || !std::isfinite(line.startWidth) || !std::isfinite(line.endWidth) || line.startWidth < 0 || line.endWidth < 0
        || !std::all_of(points.begin(), points.end(), [](const auto& value) { return Finite(value); })) return false;
    const bool worldSpace = geometry.space == LineSpace::World;
    const bool cameraFacing = line.shape == LineShape::Ribbon && line.billboard && cameraWorldPosition;
    const auto& scale = transform.worldScale;
    const auto& rotation = transform.worldRotation;
    if ((worldSpace || cameraFacing) && (!Finite(scale) || !Finite(transform.worldPosition)
        || std::abs(scale.x) <= 0.000001f || std::abs(scale.y) <= 0.000001f || std::abs(scale.z) <= 0.000001f
        || !std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(rotation.w)
        || rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z + rotation.w * rotation.w <= 0.000001f)) return false;
    if (cameraFacing && !Finite(*cameraWorldPosition)) return false;
    const auto inverse = (worldSpace || cameraFacing) ? rotation.Inverse() : math::Quaternion::Identity();
    const auto toLocal = [&](const math::Vector3& position) {
        return worldSpace ? Divide(inverse * (position - transform.worldPosition), scale) : position;
    };
    const auto localNormal = [&](const math::Vector3& normal) {
        if (!worldSpace) return normal;
        const auto rotated = inverse * normal;
        return math::Vector3{rotated.x * scale.x, rotated.y * scale.y, rotated.z * scale.z}.NormalizedOr(math::Vector3::FORWARD);
    };
    const auto localDirection = [&](const math::Vector3& direction) {
        return worldSpace ? Divide(inverse * direction, scale).NormalizedOr(math::Vector3::RIGHT) : direction;
    };
    math::Vector3 camera = math::Vector3::ZERO;
    if (cameraFacing) camera = worldSpace ? *cameraWorldPosition : Divide(inverse * (*cameraWorldPosition - transform.worldPosition), scale);
    const size_t count = points.size();
    const size_t spans = line.loop ? count : count - 1;
    std::vector<float> distances(spans + 1, 0.0f);
    for (size_t span = 0; span < spans; ++span) distances[span + 1] = distances[span] + (points[(span + 1) % count] - points[span]).Length();
    const float total = distances.back();
    if (!std::isfinite(total) || total <= 0.000001f) return false;
    const auto colorAt = [&](float t) {
        return math::Vector4{std::lerp(line.startColor.x, line.endColor.x, t), std::lerp(line.startColor.y, line.endColor.y, t),
            std::lerp(line.startColor.z, line.endColor.z, t), std::lerp(line.startColor.w, line.endColor.w, t)};
    };
    if (line.shape == LineShape::Tube) {
        const int radial = std::clamp(line.radialSegments, 3, 32);
        const auto firstAxis = (points[1] - points[0]).NormalizedOr(math::Vector3::FORWARD);
        auto normalRef = CrossSide(firstAxis, std::abs(firstAxis.y) > 0.9f ? math::Vector3::FORWARD : math::Vector3::UP);
        auto initialNormal = normalRef;
        for (size_t ring = 0; ring <= spans; ++ring) {
            const size_t index = ring % count;
            const auto back = (index > 0 || line.loop) ? points[index] - points[(index + count - 1) % count] : math::Vector3::ZERO;
            const auto forward = (index + 1 < count || line.loop) ? points[(index + 1) % count] - points[index] : math::Vector3::ZERO;
            const auto axis = (back + forward).NormalizedOr(firstAxis);
            normalRef = (normalRef - axis * math::Vector3::Dot(normalRef, axis)).NormalizedOr(CrossSide(axis, math::Vector3::UP));
            if (ring == 0) initialNormal = normalRef;
            if (line.loop && ring == spans) normalRef = initialNormal;
            const auto binormal = math::Vector3::Cross(axis, normalRef).NormalizedOr(math::Vector3::UP);
            const float t = distances[ring] / total;
            const float radius = (line.startWidth + (line.endWidth - line.startWidth) * t) * 0.5f;
            for (int step = 0; step <= radial; ++step) {
                const float angle = static_cast<float>(step) / static_cast<float>(radial) * 6.28318530718f;
                const auto outward = normalRef * std::cos(angle) + binormal * std::sin(angle);
                vertices.push_back({toLocal(points[index] + outward * radius), localNormal(outward),
                    localDirection(axis), {t, static_cast<float>(step) / static_cast<float>(radial)}, colorAt(t)});
            }
        }
        const uint32_t stride = static_cast<uint32_t>(radial + 1);
        for (size_t span = 0; span < spans; ++span) {
            if (distances[span + 1] == distances[span]) continue;
            const auto a = static_cast<uint32_t>(span) * stride;
            const auto b = a + stride;
            for (int step = 0; step < radial; ++step) {
                const auto offset = static_cast<uint32_t>(step);
                indices.insert(indices.end(), {a + offset, b + offset, b + offset + 1, a + offset, b + offset + 1, a + offset + 1});
            }
        }
    } else {
        for (size_t span = 0; span < spans; ++span) {
            const auto& a = points[span];
            const auto& b = points[(span + 1) % count];
            if (distances[span + 1] == distances[span]) continue;
            const auto direction = (b - a).NormalizedOr(math::Vector3::FORWARD);
            const auto view = cameraFacing ? (camera - (a + b) * 0.5f).NormalizedOr(math::Vector3::FORWARD) : math::Vector3::FORWARD;
            const auto side = CrossSide(direction, view);
            const auto normal = localNormal(math::Vector3::Cross(side, direction).NormalizedOr(math::Vector3::FORWARD));
            const float t0 = distances[span] / total, t1 = distances[span + 1] / total;
            const float w0 = (line.startWidth + (line.endWidth - line.startWidth) * t0) * 0.5f;
            const float w1 = (line.startWidth + (line.endWidth - line.startWidth) * t1) * 0.5f;
            const auto base = static_cast<uint32_t>(vertices.size());
            vertices.insert(vertices.end(), {
                {toLocal(a - side * w0), normal, localDirection(direction), {t0, 0}, colorAt(t0)},
                {toLocal(a + side * w0), normal, localDirection(direction), {t0, 1}, colorAt(t0)},
                {toLocal(b + side * w1), normal, localDirection(direction), {t1, 1}, colorAt(t1)},
                {toLocal(b - side * w1), normal, localDirection(direction), {t1, 0}, colorAt(t1)}
            });
            indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    }
    if (!std::all_of(vertices.begin(), vertices.end(), [](const auto& vertex) {
        return Finite(vertex.position) && Finite(vertex.normal) && Finite(vertex.tangent) && Finite(vertex.color);
    })) { vertices.clear(); indices.clear(); return false; }
    return !indices.empty();
}

bool BuildSpriteGeometry(const SpriteRendererComponent& sprite, const asset::ResolvedSprite& resolved,
    std::vector<renderer::Vertex>& vertices, std::vector<uint32_t>& indices)
{
    vertices.clear(); indices.clear();
    if (!std::isfinite(sprite.size.x) || !std::isfinite(sprite.size.y) || sprite.size.x <= 0 || sprite.size.y <= 0
        || !std::isfinite(sprite.pivot.x) || !std::isfinite(sprite.pivot.y) || !Finite(resolved.border)
        || !std::isfinite(resolved.uvMin.x) || !std::isfinite(resolved.uvMin.y)
        || !std::isfinite(resolved.uvMax.x) || !std::isfinite(resolved.uvMax.y)) return false;
    const float left = -sprite.pivot.x * sprite.size.x, top = (1 - sprite.pivot.y) * sprite.size.y;
    const float right = left + sprite.size.x, bottom = top - sprite.size.y;
    if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top) || !std::isfinite(bottom)) return false;
    const float u0 = sprite.flipX ? resolved.uvMax.x : resolved.uvMin.x;
    const float u1 = sprite.flipX ? resolved.uvMin.x : resolved.uvMax.x;
    const float v0 = sprite.flipY ? resolved.uvMax.y : resolved.uvMin.y;
    const float v1 = sprite.flipY ? resolved.uvMin.y : resolved.uvMax.y;
    if (sprite.drawMode != SpriteDrawMode::Sliced || !std::isfinite(resolved.pixelsPerUnit) || resolved.pixelsPerUnit <= 0
        || !std::isfinite(resolved.sizePixels.x) || !std::isfinite(resolved.sizePixels.y)
        || resolved.sizePixels.x <= 0 || resolved.sizePixels.y <= 0) {
        Quad(vertices, indices, left, top, right, bottom, u0, v0, u1, v1);
        return true;
    }
    std::array<float, 4> x, y, u, v;
    SliceAxis(left, right, u0, u1, resolved.sizePixels.x, sprite.flipX ? resolved.border.z : resolved.border.x,
        sprite.flipX ? resolved.border.x : resolved.border.z, resolved.pixelsPerUnit, x, u);
    SliceAxis(top, bottom, v0, v1, resolved.sizePixels.y, sprite.flipY ? resolved.border.w : resolved.border.y,
        sprite.flipY ? resolved.border.y : resolved.border.w, resolved.pixelsPerUnit, y, v);
    for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 3; ++column)
            if (x[column + 1] > x[column] && y[row] > y[row + 1])
                Quad(vertices, indices, x[column], y[row], x[column + 1], y[row + 1], u[column], v[row], u[column + 1], v[row + 1]);
    return !indices.empty();
}
} /// @note namespace fbzz::scene
