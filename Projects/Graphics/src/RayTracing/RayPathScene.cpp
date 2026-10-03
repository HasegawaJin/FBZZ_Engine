/// @file    RayPathScene.cpp
/// @brief   Path の実内容比較と版付き CPU 三角形からの発光面分布構築。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <Graphics/RayTracing/RayPathScene.hpp>
#include <Graphics/Renderer/IBuffer.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>

namespace fbzz::renderer {
namespace {

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Nonnegative(const math::Vector3& value)
{
    return Finite(value) && value.x >= 0 && value.y >= 0 && value.z >= 0;
}

double TransformDeterminant(const math::Matrix4& transform)
{
    const auto& m = transform.m;
    return static_cast<double>(m[0][0]) * (static_cast<double>(m[1][1]) * m[2][2] - static_cast<double>(m[1][2]) * m[2][1])
        - static_cast<double>(m[0][1]) * (static_cast<double>(m[1][0]) * m[2][2] - static_cast<double>(m[1][2]) * m[2][0])
        + static_cast<double>(m[0][2]) * (static_cast<double>(m[1][0]) * m[2][1] - static_cast<double>(m[1][1]) * m[2][0]);
}

bool ValidTransform(const math::Matrix4& transform)
{
    for (const auto& row : transform.m)
        for (float value : row)
            if (!std::isfinite(value)) return false;
    if (transform.m[3][0] != 0 || transform.m[3][1] != 0
        || transform.m[3][2] != 0 || transform.m[3][3] != 1) return false;
    const double determinant = TransformDeterminant(transform);
    return std::isfinite(determinant) && determinant != 0;
}

void Append(std::vector<uint32_t>& key, uint64_t value)
{
    key.push_back(static_cast<uint32_t>(value));
    key.push_back(static_cast<uint32_t>(value >> 32));
}

void Append(std::vector<uint32_t>& key, float value)
{
    key.push_back(std::bit_cast<uint32_t>(value));
}

void Append(std::vector<uint32_t>& key, const math::Vector3& value)
{
    Append(key, value.x); Append(key, value.y); Append(key, value.z);
}

void Append(std::vector<uint32_t>& key, const RayObjectId& value)
{
    Append(key, value.sceneGeneration);
    key.push_back(value.index); key.push_back(value.generation);
}

void Append(std::vector<uint32_t>& words, const RayGeometryKey& key)
{
    words.push_back(key.vertices.id); words.push_back(key.vertices.gen);
    words.push_back(key.indices.id); words.push_back(key.indices.gen);
    Append(words, key.vertexContentVersion); Append(words, key.indexContentVersion);
    words.push_back(key.vertexStride); words.push_back(key.firstVertex); words.push_back(key.vertexCount);
    words.push_back(key.positionOffset); words.push_back(key.firstIndex); words.push_back(key.indexCount);
    words.push_back(key.opaque); words.push_back(key.doubleSided);
}

void Append(std::vector<uint32_t>& key, const RayLightInput& light, bool respectArtistShadows)
{
    Append(key, light.objectId);
    key.push_back(light.layerMask); key.push_back(static_cast<uint32_t>(light.type));
    Append(key, light.position); Append(key, light.direction);
    Append(key, light.tangent); Append(key, light.bitangent); Append(key, light.color);
    Append(key, light.intensity); Append(key, light.range);
    Append(key, light.innerCone); Append(key, light.outerCone);
    Append(key, light.areaWidth); Append(key, light.areaHeight);
    Append(key, light.sourceRadius); Append(key, light.sourceLength);
    key.push_back(light.twoSided); key.push_back(light.unsupportedFlags);
    if (respectArtistShadows) { key.push_back(light.castShadows); Append(key, light.shadowStrength); }
}

std::vector<uint32_t> ContentKey(const RayScene& scene, const RayPathScene& path, bool respectArtistShadows)
{
    std::vector<uint32_t> key;
    Append(key, scene.sceneGeneration);
    key.push_back(scene.layerMask);
    Append(key, static_cast<uint64_t>(scene.geometries.size()));
    for (const auto& geometry : scene.geometries) Append(key, geometry.key);
    Append(key, static_cast<uint64_t>(scene.instances.size()));
    for (const auto& instance : scene.instances) {
        Append(key, instance.objectId);
        key.push_back(instance.geometryIndex); key.push_back(instance.denseInstanceId);
        key.push_back(instance.sourceSubmesh); key.push_back(instance.materialSlot);
        /// @note Reference の全 transport query は PRIMARY だけを消費し、Hybrid 用 opt-out では蓄積を捨てない。
        key.push_back(respectArtistShadows ? instance.mask : instance.mask & RAY_PRIMARY_MASK);
        key.push_back(instance.doubleSided);
        for (const auto& row : instance.world.m)
            for (float value : row) Append(key, value);
        const auto& surface = instance.surface;
        Append(key, surface.baseColor.x); Append(key, surface.baseColor.y);
        Append(key, surface.baseColor.z); Append(key, surface.baseColor.w);
        Append(key, surface.emission); Append(key, surface.metallic); Append(key, surface.roughness);
        key.push_back(surface.standardSurfaceSupported); key.push_back(static_cast<uint32_t>(surface.issue));
        Append(key, surface.dielectric.transmission); Append(key, surface.dielectric.ior);
        Append(key, surface.dielectric.attenuationColor); Append(key, surface.dielectric.attenuationDistance);
        key.push_back(surface.dielectric.thinWalled); key.push_back(surface.solidDielectricSupported);
        for (const auto& texture : surface.textures) {
            key.push_back(texture.texture.id); key.push_back(texture.texture.gen);
            Append(key, texture.contentVersion);
        }
        for (float value : surface.uvTiling) Append(key, value);
        for (float value : surface.uvOffset) Append(key, value);
        Append(key, surface.normalStrength); Append(key, surface.occlusionStrength);
        Append(key, surface.alphaCutoff); Append(key, surface.explicitTextureLod);
        key.push_back(surface.textureMask);
    }
    key.push_back(path.lighting.useSceneLights);
    key.push_back(respectArtistShadows);
    if (path.lighting.useSceneLights) {
        Append(key, static_cast<uint64_t>(std::count_if(path.lighting.lights.begin(), path.lighting.lights.end(),
            [&](const RayLightInput& light) { return (light.layerMask & scene.layerMask) != 0
                || (light.unsupportedFlags & RAY_LIGHT_INVALID_LAYER) != 0; })));
        for (const auto& light : path.lighting.lights)
            if ((light.layerMask & scene.layerMask) != 0 || (light.unsupportedFlags & RAY_LIGHT_INVALID_LAYER) != 0)
                Append(key, light, respectArtistShadows);
    } else {
        Append(key, path.lighting.direction); Append(key, path.lighting.directionalRadiance);
    }
    Append(key, path.lighting.environmentRadiance); Append(key, path.lighting.environmentContentVersion);
    Append(key, path.lighting.environmentRotation); Append(key, path.lighting.environmentIntensity);
    key.push_back(path.lighting.supported);
    Append(key, static_cast<uint64_t>(scene.diagnostics.size()));
    for (const auto& diagnostic : scene.diagnostics) {
        Append(key, diagnostic.objectId); key.push_back(static_cast<uint32_t>(diagnostic.issue));
    }
    Append(key, static_cast<uint64_t>(scene.surfaceDiagnostics.size()));
    for (const auto& diagnostic : scene.surfaceDiagnostics) {
        Append(key, diagnostic.objectId); key.push_back(static_cast<uint32_t>(diagnostic.issue));
    }
    Append(key, static_cast<uint64_t>(path.diagnostics.size()));
    for (const auto& diagnostic : path.diagnostics) {
        key.push_back(diagnostic.instanceId); key.push_back(static_cast<uint32_t>(diagnostic.issue));
        Append(key, diagnostic.objectId);
    }
    return key;
}

bool MatchesTriangleInput(const RaySceneGeometry& geometry)
{
    const auto& key = geometry.key;
    const auto& input = geometry.triangles;
    return key.vertices == input.vertices && key.indices == input.indices
        && key.firstVertex == input.firstVertex && key.vertexCount == input.vertexCount
        && key.positionOffset == input.positionOffset && key.firstIndex == input.firstIndex
        && key.indexCount == input.indexCount && key.opaque == input.opaque;
}

bool ValidBufferVersions(const RayGeometryKey& key, const RayPathBufferLookup& lookup,
    RayPathSceneIssue& issue)
{
    const auto* vertices = lookup ? lookup(key.vertices) : nullptr;
    const auto* indices = key.indices.IsValid() && lookup ? lookup(key.indices) : nullptr;
    if (!vertices || (key.indices.IsValid() && !indices)) {
        issue = RayPathSceneIssue::BUFFER_READER_UNSUPPORTED;
        return false;
    }
    if (key.vertexContentVersion == 0 || vertices->GetContentVersion() != key.vertexContentVersion
        || (indices && (key.indexContentVersion == 0 || indices->GetContentVersion() != key.indexContentVersion))) {
        issue = RayPathSceneIssue::BUFFER_CONTENT_MISMATCH;
        return false;
    }
    return true;
}

bool MakeEmitter(const std::array<math::Vector3, 3>& local, const RaySceneInstance& instance,
    uint32_t primitiveId, RayPathEmitterRecord& record)
{
    std::array<math::Vector3, 3> world;
    for (size_t i = 0; i < world.size(); ++i) {
        const auto value = instance.world * math::Vector4{local[i].x, local[i].y, local[i].z, 1};
        world[i] = {value.x, value.y, value.z};
        if (!Finite(world[i])) return false;
    }
    const math::Vector3 edge1 = world[1] - world[0];
    const math::Vector3 edge2 = world[2] - world[0];
    if (!Finite(edge1) || !Finite(edge2)) return false;
    const std::array<double, 3> cross{
        static_cast<double>(edge1.y) * edge2.z - static_cast<double>(edge1.z) * edge2.y,
        static_cast<double>(edge1.z) * edge2.x - static_cast<double>(edge1.x) * edge2.z,
        static_cast<double>(edge1.x) * edge2.y - static_cast<double>(edge1.y) * edge2.x};
    const double length = std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
    const double doubleArea = 0.5 * length;
    if (!std::isfinite(doubleArea) || doubleArea > std::numeric_limits<float>::max()) return false;
    const float area = static_cast<float>(doubleArea);
    if (!std::isfinite(area) || area <= 0) return false;
    record.v0 = {world[0].x, world[0].y, world[0].z};
    record.area = area;
    record.edge1 = {edge1.x, edge1.y, edge1.z};
    record.instanceId = instance.denseInstanceId;
    record.edge2 = {edge2.x, edge2.y, edge2.z};
    record.primitiveId = primitiveId;
    record.emission = {instance.surface.emission.x, instance.surface.emission.y, instance.surface.emission.z};
    record.objectIndex = instance.objectId.index;
    record.objectGeneration = instance.objectId.generation;
    /// @note DXR の front 面は instance transform で反転しない。cross(M e1, M e2) の determinant 符号を取り除く。
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_raytracing_instance_flags DXR instance winding
    const double winding = TransformDeterminant(instance.world) < 0 ? -1.0 : 1.0;
    record.geometricNormal = {static_cast<float>(winding * cross[0] / length),
        static_cast<float>(winding * cross[1] / length), static_cast<float>(winding * cross[2] / length)};
    return true;
}

double EmissionSides(const RayPathEmitterRecord& emitter)
{
    return (emitter.flags & RAY_PATH_EMITTER_TWO_SIDED) != 0 ? 2.0 : 1.0;
}

double EmissionSides(const RayPathShapeRecord&) { return 1.0; }

template<class Record>
bool NormalizeSelection(std::vector<Record>& emitters)
{
    if (emitters.empty()) return true;
    std::vector<double> weights;
    double total = 0;
    for (const auto& emitter : emitters) {
        /// @note 同一 radiance の発光面は面積比例。RGB の重みは線形 Rec.709 luminance。
        /// @see https://pbr-book.org/4ed/Light_Sources/Light_Sampling PBRT, power light sampler
        const double luminance = 0.2126 * emitter.emission[0] + 0.7152 * emitter.emission[1] + 0.0722 * emitter.emission[2];
        const double weight = static_cast<double>(emitter.area) * luminance * EmissionSides(emitter);
        if (!std::isfinite(weight) || weight < 0) return false;
        weights.push_back(weight);
        total += weight;
    }
    if (!std::isfinite(total)) return false;
    if (total == 0) return true;
    size_t lastPositive = weights.size() - 1;
    while (weights[lastPositive] == 0) --lastPositive;
    double cumulative = 0;
    float previous = 0;
    for (size_t i = 0; i < emitters.size(); ++i) {
        cumulative += weights[i];
        const float cdf = i >= lastPositive ? 1.0f : static_cast<float>(cumulative / total);
        const float pdf = cdf - previous;
        /// @note float CDF の実際の区間を PDF として保存し、MIS の逆引きと選択分布を一致させる。
        if (!std::isfinite(pdf) || pdf < 0 || (weights[i] > 0 && pdf == 0) || cdf > 1) return false;
        emitters[i].selectionPdf = pdf;
        emitters[i].selectionCdf = cdf;
        previous = cdf;
    }
    return true;
}

std::array<float, 3> Values(const math::Vector3& value)
{
    return {value.x, value.y, value.z};
}

math::Vector3 Vector(const std::array<float, 3>& value)
{
    return {value[0], value[1], value[2]};
}

bool Normalize(const math::Vector3& input, math::Vector3& output)
{
    if (!Finite(input)) return false;
    const double length = std::sqrt(static_cast<double>(input.x) * input.x
        + static_cast<double>(input.y) * input.y + static_cast<double>(input.z) * input.z);
    if (!std::isfinite(length) || length == 0) return false;
    output = {static_cast<float>(input.x / length), static_cast<float>(input.y / length),
        static_cast<float>(input.z / length)};
    return Finite(output);
}

bool Radiance(const RayLightInput& light, math::Vector3& radiance)
{
    if (!Nonnegative(light.color) || !std::isfinite(light.intensity) || light.intensity < 0) return false;
    const double scale = static_cast<double>(light.intensity)
        * (light.type == RayLightType::AREA ? 1.0 : 3.14159265358979323846);
    const std::array<double, 3> value{light.color.x * scale, light.color.y * scale, light.color.z * scale};
    for (double channel : value)
        if (!std::isfinite(channel) || channel > std::numeric_limits<float>::max()) return false;
    radiance = {static_cast<float>(value[0]), static_cast<float>(value[1]), static_cast<float>(value[2])};
    return true;
}

bool ValidLight(RayLightInput& light, math::Vector3& radiance)
{
    if (!std::isfinite(light.shadowStrength)) return false;
    if (!std::isfinite(light.range) || light.range < 0) return false;
    if (light.type == RayLightType::SPHERE || light.type == RayLightType::TUBE)
        return Nonnegative(light.color) && std::isfinite(light.intensity) && light.intensity >= 0
            && Finite(light.position) && std::isfinite(light.sourceRadius) && light.sourceRadius > 0
            && (light.type == RayLightType::SPHERE || (std::isfinite(light.sourceLength)
                && light.sourceLength >= 0 && Normalize(light.tangent, light.tangent)));
    if (!Radiance(light, radiance)) return false;
    if (light.type == RayLightType::DIRECTIONAL) return Normalize(light.direction, light.direction);
    if (!Finite(light.position)) return false;
    if (light.type == RayLightType::POINT) return true;
    if (!Normalize(light.direction, light.direction)) return false;
    if (light.type == RayLightType::SPOT)
        return std::isfinite(light.innerCone) && std::isfinite(light.outerCone)
            && light.innerCone >= 0 && light.innerCone <= light.outerCone && light.outerCone < 90;
    if (light.type != RayLightType::AREA || !std::isfinite(light.areaWidth) || light.areaWidth <= 0
        || !std::isfinite(light.areaHeight) || light.areaHeight <= 0
        || !Normalize(light.tangent, light.tangent) || !Normalize(light.bitangent, light.bitangent)) return false;
    return std::abs(math::Vector3::Dot(light.direction, light.tangent)) < 1e-5f
        && std::abs(math::Vector3::Dot(light.direction, light.bitangent)) < 1e-5f
        && std::abs(math::Vector3::Dot(light.tangent, light.bitangent)) < 1e-5f;
}

bool MakeShape(const RayLightInput& light, RayPathShapeRecord& record)
{
    constexpr double pi = 3.14159265358979323846;
    const double radius = light.sourceRadius;
    const double length = light.type == RayLightType::TUBE ? light.sourceLength : 0.0;
    const double area = 4 * pi * radius * radius + 2 * pi * radius * length;
    if (!std::isfinite(area) || area <= 0 || area > std::numeric_limits<float>::max()) return false;
    record.area = static_cast<float>(area);
    if (record.area == 0) return false;
    const double scale = 4 * pi * light.intensity / area;
    const std::array<double, 3> emission{light.color.x * scale, light.color.y * scale, light.color.z * scale};
    for (double channel : emission)
        if (!std::isfinite(channel) || channel > std::numeric_limits<float>::max()) return false;
    record.position = Values(light.position);
    record.radius = light.sourceRadius;
    record.axis = Values(light.type == RayLightType::TUBE ? light.tangent : math::Vector3{0, 1, 0});
    record.halfLength = static_cast<float>(length * 0.5);
    record.emission = {static_cast<float>(emission[0]), static_cast<float>(emission[1]), static_cast<float>(emission[2])};
    record.range = light.range;
    record.type = light.type == RayLightType::SPHERE ? 0u : 1u;
    record.objectIndex = light.objectId.index;
    record.objectGeneration = light.objectId.generation;
    record.shadowStrength = light.castShadows ? std::clamp(light.shadowStrength, 0.0f, 1.0f) : 0;
    return true;
}

bool SameOwner(const RayPathEmitterRecord& emitter, const RayLightInput& light)
{
    return emitter.objectIndex == light.objectId.index && emitter.objectGeneration == light.objectId.generation;
}

double Projection(const math::Vector3& point, const math::Vector3& origin, const math::Vector3& axis)
{
    return (static_cast<double>(point.x) - origin.x) * axis.x
        + (static_cast<double>(point.y) - origin.y) * axis.y
        + (static_cast<double>(point.z) - origin.z) * axis.z;
}

/// @note 初期 proxy は矩形四隅と一つの diagonal を持つ共面二面だけ。穴・重複・部分 patch を面積一致で誤認しない。
bool BindAreaMesh(const RayLightInput& light, const math::Vector3& radiance,
    std::vector<RayPathEmitterRecord>& emitters)
{
    /// @note 薄い立体の裏面遮蔽を無視して両面発光にはできない。両面 proxy は全所有 geometry の証明まで未対応。
    if (light.twoSided) return false;
    std::vector<size_t> selected;
    for (size_t i = 0; i < emitters.size(); ++i)
        if (SameOwner(emitters[i], light)
            && math::Vector3::Dot(Vector(emitters[i].geometricNormal), light.direction) > 1 - 1e-5f)
            selected.push_back(i);
    if (selected.size() != 2) return false;
    /// @note 絶対 floor は小光源の寸法と薄さを誤認する。保存された float 頂点の差と投影を double で検証する。
    const double minimumDimension = (std::min)(light.areaWidth, light.areaHeight);
    const double tolerance = minimumDimension * 1e-4;
    if (!std::isfinite(tolerance) || tolerance <= 0) return false;
    std::array<std::array<uint32_t, 4>, 4> edges{};
    std::array<uint32_t, 4> corners{};
    double plane = 0;
    bool first = true;
    for (size_t index : selected) {
        const auto& emitter = emitters[index];
        const auto v0 = Vector(emitter.v0);
        const std::array<math::Vector3, 3> vertices{v0, v0 + Vector(emitter.edge1), v0 + Vector(emitter.edge2)};
        std::array<uint32_t, 3> ids{};
        for (size_t i = 0; i < vertices.size(); ++i) {
            if (!Finite(vertices[i])) return false;
            const double x = Projection(vertices[i], light.position, light.tangent);
            const double y = Projection(vertices[i], light.position, light.bitangent);
            const double z = Projection(vertices[i], light.position, light.direction);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)
                || std::abs(std::abs(x) - static_cast<double>(light.areaWidth) * 0.5) > tolerance
                || std::abs(std::abs(y) - static_cast<double>(light.areaHeight) * 0.5) > tolerance) return false;
            if (first) { plane = z; first = false; }
            if (std::abs(z - plane) > tolerance) return false;
            ids[i] = (x > 0 ? 1u : 0u) | (y > 0 ? 2u : 0u);
            ++corners[ids[i]];
        }
        if (ids[0] == ids[1] || ids[1] == ids[2] || ids[2] == ids[0]) return false;
        for (size_t i = 0; i < ids.size(); ++i) {
            const uint32_t a = (std::min)(ids[i], ids[(i + 1) % ids.size()]);
            const uint32_t b = (std::max)(ids[i], ids[(i + 1) % ids.size()]);
            ++edges[a][b];
        }
    }
    /// @note 薄板の forward 境界を実発光面にする。中心より裏や厚みが短辺の 10% を超える形状は曖昧とする。
    if (plane < -tolerance || plane > minimumDimension * 0.05 + tolerance) return false;
    for (uint32_t count : corners) if (count == 0) return false;
    if (edges[0][1] != 1 || edges[0][2] != 1 || edges[1][3] != 1 || edges[2][3] != 1
        || !((edges[0][3] == 2 && edges[1][2] == 0) || (edges[0][3] == 0 && edges[1][2] == 2))) return false;
    for (const auto& emitter : emitters) {
        if (!SameOwner(emitter, light)) continue;
        const auto v0 = Vector(emitter.v0);
        for (const auto& vertex : std::array<math::Vector3, 3>{v0, v0 + Vector(emitter.edge1), v0 + Vector(emitter.edge2)}) {
            if (!Finite(vertex)) return false;
            const double projection = Projection(vertex, light.position, light.direction);
            if (!std::isfinite(projection) || projection > plane + tolerance) return false;
        }
    }
    std::vector<RayPathEmitterRecord> retained;
    retained.reserve(emitters.size());
    for (size_t i = 0; i < emitters.size(); ++i) {
        if (!SameOwner(emitters[i], light)) { retained.push_back(emitters[i]); continue; }
        if (std::find(selected.begin(), selected.end(), i) == selected.end()) continue;
        auto emitter = emitters[i];
        emitter.emission = Values(radiance);
        emitter.range = light.range;
        emitter.flags = RAY_PATH_EMITTER_MESH_LIGHT_PROXY | (light.twoSided ? RAY_PATH_EMITTER_TWO_SIDED : 0u);
        emitter.shadowStrength = light.castShadows ? std::clamp(light.shadowStrength, 0.0f, 1.0f) : 0;
        retained.push_back(emitter);
    }
    emitters = std::move(retained);
    return true;
}

bool MakeVirtualArea(const RayLightInput& light, const math::Vector3& radiance,
    std::vector<RayPathEmitterRecord>& emitters, uint32_t& primitiveId)
{
    const auto t = light.tangent * (light.areaWidth * 0.5f);
    const auto b = light.bitangent * (light.areaHeight * 0.5f);
    std::array<math::Vector3, 4> vertices{light.position - t - b, light.position + t - b,
        light.position + t + b, light.position - t + b};
    if (math::Vector3::Dot(math::Vector3::Cross(t, b), light.direction) < 0) std::swap(vertices[1], vertices[3]);
    RaySceneInstance instance;
    instance.objectId = light.objectId;
    instance.denseInstanceId = UINT32_MAX;
    instance.surface.emission = radiance;
    for (uint32_t i = 0; i < 2; ++i) {
        RayPathEmitterRecord emitter;
        if (!MakeEmitter({vertices[0], vertices[i + 1], vertices[i + 2]}, instance, primitiveId, emitter)
            || primitiveId == UINT32_MAX) return false;
        ++primitiveId;
        emitter.range = light.range;
        emitter.flags = RAY_PATH_EMITTER_VIRTUAL | (light.twoSided ? RAY_PATH_EMITTER_TWO_SIDED : 0u);
        emitter.shadowStrength = light.castShadows ? std::clamp(light.shadowStrength, 0.0f, 1.0f) : 0;
        emitters.push_back(emitter);
    }
    return true;
}

} /// @note namespace

bool RayPathSceneBuilder::ReadGeometry(const RayGeometryKey& key, const RayPathBufferLookup& lookup,
    std::vector<LocalTriangle>& triangles, RayPathSceneIssue& issue) const
{
    issue = RayPathSceneIssue::INVALID_EMITTER_GEOMETRY;
    if (!ValidBufferVersions(key, lookup, issue)) return false;
    const auto* vertices = lookup(key.vertices);
    const auto* indices = key.indices.IsValid() ? lookup(key.indices) : nullptr;
    if (!key.vertices.IsValid() || key.vertexStride < 12 || key.vertexStride % 4 != 0
        || key.positionOffset % 4 != 0 || key.positionOffset > key.vertexStride - 12
        || vertices->GetStride() != key.vertexStride || key.vertexCount == 0
        || (indices ? key.indexCount == 0 || key.indexCount % 3 != 0
                    : key.indexCount != 0 || key.vertexCount % 3 != 0)) return false;
    const uint64_t endVertex = static_cast<uint64_t>(key.firstVertex) + key.vertexCount;
    if (endVertex > std::numeric_limits<uint64_t>::max() / key.vertexStride) return false;
    const uint64_t vertexEnd = endVertex * key.vertexStride;
    if (vertexEnd > vertices->GetSize() || vertexEnd > std::numeric_limits<size_t>::max()) return false;
    uint8_t lastByte = 0;
    if (!vertices->CopyData(static_cast<size_t>(vertexEnd - 1), 1, &lastByte)) {
        issue = RayPathSceneIssue::BUFFER_READER_UNSUPPORTED;
        return false;
    }
    std::vector<math::Vector3> positions(key.vertexCount);
    for (uint32_t i = 0; i < key.vertexCount; ++i) {
        const uint64_t offset = (static_cast<uint64_t>(key.firstVertex) + i) * key.vertexStride + key.positionOffset;
        std::array<float, 3> position{};
        if (!vertices->CopyData(static_cast<size_t>(offset), sizeof(position), position.data())) {
            issue = RayPathSceneIssue::BUFFER_READER_UNSUPPORTED;
            return false;
        }
        positions[i] = {position[0], position[1], position[2]};
        if (!Finite(positions[i])) return false;
    }
    std::vector<uint32_t> indexValues;
    if (indices) {
        const uint64_t indexOffset = static_cast<uint64_t>(key.firstIndex) * sizeof(uint32_t);
        const uint64_t indexSize = static_cast<uint64_t>(key.indexCount) * sizeof(uint32_t);
        if (indexOffset > indices->GetSize() || indexSize > indices->GetSize() - indexOffset
            || indexSize > std::numeric_limits<size_t>::max()) return false;
        indexValues.resize(key.indexCount);
        if (!indices->CopyData(static_cast<size_t>(indexOffset), static_cast<size_t>(indexSize), indexValues.data())) {
            issue = RayPathSceneIssue::BUFFER_READER_UNSUPPORTED;
            return false;
        }
        for (uint32_t index : indexValues)
            if (index >= key.vertexCount) return false;
    }
    const uint32_t count = (indices ? key.indexCount : key.vertexCount) / 3;
    triangles.reserve(count);
    for (uint32_t primitive = 0; primitive < count; ++primitive) {
        LocalTriangle triangle;
        triangle.primitiveId = primitive;
        for (uint32_t vertex = 0; vertex < 3; ++vertex) {
            const uint32_t index = primitive * 3 + vertex;
            triangle.vertices[vertex] = positions[indices ? indexValues[index] : index];
        }
        triangles.push_back(triangle);
    }
    return true;
}

RayPathScene RayPathSceneBuilder::Build(const RayScene& scene, ResourceManager& resources,
    const RayPathLighting& lighting, bool respectArtistShadows)
{
    return Build(scene, [&resources](ResourceHandle<BufferTag> handle) { return resources.Get(handle); }, lighting, respectArtistShadows);
}

RayPathScene RayPathSceneBuilder::Build(const RayScene& scene, const RayPathBufferLookup& lookup,
    const RayPathLighting& lighting, bool respectArtistShadows)
{
    RayPathScene result;
    result.sceneGeneration = scene.sceneGeneration;
    result.lighting = lighting;
    const auto diagnose = [&](uint32_t instanceId, RayPathSceneIssue issue, RayObjectId owner = {}) {
        result.diagnostics.push_back({instanceId, issue, owner});
    };
    if (scene.sceneGeneration == 0 || !scene.diagnostics.empty())
        diagnose(UINT32_MAX, RayPathSceneIssue::RAY_SCENE_COVERAGE_INCOMPLETE);
    if (!scene.surfaceDiagnostics.empty()) diagnose(UINT32_MAX, RayPathSceneIssue::SURFACE_UNSUPPORTED);
    if (!lighting.supported) diagnose(UINT32_MAX, RayPathSceneIssue::LIGHTING_UNSUPPORTED);
    const double directionLength = static_cast<double>(lighting.direction.x) * lighting.direction.x
        + static_cast<double>(lighting.direction.y) * lighting.direction.y
        + static_cast<double>(lighting.direction.z) * lighting.direction.z;
    const bool directionalEnabled = lighting.directionalRadiance.x > 0 || lighting.directionalRadiance.y > 0
        || lighting.directionalRadiance.z > 0;
    if ((!lighting.useSceneLights && (!Finite(lighting.direction) || (directionalEnabled && directionLength == 0)
        || !Nonnegative(lighting.directionalRadiance))) || !Nonnegative(lighting.environmentRadiance))
        diagnose(UINT32_MAX, RayPathSceneIssue::INVALID_LIGHTING);
    if (!std::isfinite(lighting.environmentRotation) || !std::isfinite(lighting.environmentIntensity)
        || lighting.environmentIntensity < 0) diagnose(UINT32_MAX, RayPathSceneIssue::INVALID_LIGHTING);
    std::vector<RayLightInput> areaLights;
    std::vector<RayObjectId> lightOwners;
    if (lighting.useSceneLights) {
        for (auto light : lighting.lights) {
            if (!respectArtistShadows) { light.castShadows = true; light.shadowStrength = 1; }
            if ((light.unsupportedFlags & RAY_LIGHT_INVALID_LAYER) != 0 || light.layerMask == 0) {
                diagnose(UINT32_MAX, RayPathSceneIssue::INVALID_LIGHT_SOURCE, light.objectId);
                continue;
            }
            if ((light.layerMask & scene.layerMask) == 0) continue;
            if (light.objectId.sceneGeneration != scene.sceneGeneration
                || std::find(lightOwners.begin(), lightOwners.end(), light.objectId) != lightOwners.end()) {
                diagnose(UINT32_MAX, RayPathSceneIssue::INVALID_LIGHT_SOURCE, light.objectId);
                continue;
            }
            lightOwners.push_back(light.objectId);
            if (light.unsupportedFlags != 0) {
                diagnose(UINT32_MAX, RayPathSceneIssue::UNSUPPORTED_LIGHT_SOURCE, light.objectId);
                continue;
            }
            math::Vector3 radiance;
            if (!ValidLight(light, radiance)) {
                diagnose(UINT32_MAX, RayPathSceneIssue::INVALID_LIGHT_SOURCE, light.objectId);
                continue;
            }
            if (light.type == RayLightType::AREA) { areaLights.push_back(light); continue; }
            if (light.type == RayLightType::SPHERE || light.type == RayLightType::TUBE) {
                RayPathShapeRecord shape;
                const bool hasMesh = std::any_of(scene.instances.begin(), scene.instances.end(),
                    [&](const RaySceneInstance& instance) { return instance.objectId == light.objectId; });
                if (hasMesh) diagnose(UINT32_MAX, RayPathSceneIssue::SHAPE_MESH_BINDING_UNSUPPORTED, light.objectId);
                else if (!MakeShape(light, shape)) diagnose(UINT32_MAX, RayPathSceneIssue::INVALID_LIGHT_SOURCE, light.objectId);
                else result.shapes.push_back(shape);
                continue;
            }
            RayPathDeltaRecord record;
            record.position = Values(light.position);
            record.range = light.type == RayLightType::DIRECTIONAL ? 0 : light.range;
            record.radiance = Values(radiance);
            record.direction = Values(light.direction);
            record.type = light.type == RayLightType::POINT ? 0u : light.type == RayLightType::SPOT ? 1u : 2u;
            record.shadowStrength = light.castShadows ? std::clamp(light.shadowStrength, 0.0f, 1.0f) : 0;
            if (light.type == RayLightType::SPOT) {
                constexpr double degrees = 3.14159265358979323846 / 180.0;
                record.innerCos = static_cast<float>(std::cos(light.innerCone * degrees));
                record.outerCos = static_cast<float>(std::cos(light.outerCone * degrees));
                if (light.innerCone != light.outerCone && record.innerCos <= record.outerCos) {
                    diagnose(UINT32_MAX, RayPathSceneIssue::INVALID_LIGHT_SOURCE, light.objectId);
                    continue;
                }
            }
            result.deltaLights.push_back(record);
        }
    }
    std::vector<CachedGeometry> activeGeometry;
    for (size_t i = 0; i < scene.instances.size(); ++i) {
        const auto& instance = scene.instances[i];
        if (instance.denseInstanceId != i || instance.geometryIndex >= scene.geometries.size()
            || !ValidTransform(instance.world)) {
            diagnose(instance.denseInstanceId, RayPathSceneIssue::INVALID_INSTANCE);
            continue;
        }
        if (MakeRaySurfaceRecord(instance.surface).supported == 0) {
            diagnose(instance.denseInstanceId, RayPathSceneIssue::SURFACE_UNSUPPORTED);
            continue;
        }
        if (!Nonnegative(instance.surface.emission)) {
            diagnose(instance.denseInstanceId, RayPathSceneIssue::INVALID_EMISSION);
            continue;
        }
        const bool areaProxy = std::any_of(areaLights.begin(), areaLights.end(),
            [&](const RayLightInput& light) { return light.objectId == instance.objectId; });
        if (instance.surface.emission.x == 0 && instance.surface.emission.y == 0 && instance.surface.emission.z == 0
            && !areaProxy) continue;
        if (areaProxy && (!instance.surface.standardSurfaceSupported || (instance.surface.textureMask & 8u) != 0)) {
            diagnose(instance.denseInstanceId, RayPathSceneIssue::AREA_MESH_BINDING_UNSUPPORTED, instance.objectId);
            continue;
        }
        const auto& geometry = scene.geometries[instance.geometryIndex];
        if (areaProxy && !geometry.key.opaque) {
            diagnose(instance.denseInstanceId, RayPathSceneIssue::AREA_MESH_BINDING_UNSUPPORTED, instance.objectId);
            continue;
        }
        if ((instance.doubleSided || geometry.key.doubleSided) && !areaProxy) {
            diagnose(instance.denseInstanceId, RayPathSceneIssue::EMITTER_DOUBLE_SIDED_UNSUPPORTED);
            continue;
        }
        if (!MatchesTriangleInput(geometry)) {
            diagnose(instance.denseInstanceId, RayPathSceneIssue::INVALID_EMITTER_GEOMETRY);
            continue;
        }
        RayPathSceneIssue issue = RayPathSceneIssue::INVALID_EMITTER_GEOMETRY;
        if (!ValidBufferVersions(geometry.key, lookup, issue)) {
            diagnose(instance.denseInstanceId, issue);
            continue;
        }
        auto active = std::find_if(activeGeometry.begin(), activeGeometry.end(),
            [&](const CachedGeometry& cached) { return cached.key == geometry.key; });
        if (active == activeGeometry.end()) {
            const auto cached = std::find_if(m_geometries.begin(), m_geometries.end(),
                [&](const CachedGeometry& old) { return old.key == geometry.key; });
            if (cached != m_geometries.end()) activeGeometry.push_back(*cached);
            else {
                CachedGeometry local{geometry.key, {}};
                if (!ReadGeometry(geometry.key, lookup, local.triangles, issue)) {
                    diagnose(instance.denseInstanceId, issue);
                    continue;
                }
                activeGeometry.push_back(std::move(local));
            }
            active = activeGeometry.end() - 1;
        }
        for (const auto& triangle : active->triangles) {
            RayPathEmitterRecord emitter;
            if (!MakeEmitter(triangle.vertices, instance, triangle.primitiveId, emitter)) {
                diagnose(instance.denseInstanceId, RayPathSceneIssue::INVALID_EMITTER_GEOMETRY);
                break;
            }
            result.emitters.push_back(emitter);
        }
    }
    m_geometries = std::move(activeGeometry);
    uint32_t virtualPrimitiveId = 0;
    for (const auto& light : areaLights) {
        math::Vector3 radiance;
        const bool validRadiance = Radiance(light, radiance);
        const bool hasMesh = std::any_of(result.emitters.begin(), result.emitters.end(),
            [&](const RayPathEmitterRecord& emitter) { return SameOwner(emitter, light); });
        if (!validRadiance || (hasMesh ? !BindAreaMesh(light, radiance, result.emitters)
            : !MakeVirtualArea(light, radiance, result.emitters, virtualPrimitiveId)))
            diagnose(UINT32_MAX, hasMesh ? RayPathSceneIssue::AREA_MESH_BINDING_UNSUPPORTED
                : RayPathSceneIssue::INVALID_LIGHT_SOURCE, light.objectId);
    }
    if (!NormalizeSelection(result.emitters)) diagnose(UINT32_MAX, RayPathSceneIssue::UNREPRESENTABLE_SELECTION_PDF);
    if (!NormalizeSelection(result.shapes)) diagnose(UINT32_MAX, RayPathSceneIssue::UNREPRESENTABLE_SELECTION_PDF);
    result.coverageComplete = result.diagnostics.empty();
    auto key = ContentKey(scene, result, respectArtistShadows);
    if (m_contentRevision == 0 || key != m_contentKey) {
        assert(m_contentRevision != std::numeric_limits<uint64_t>::max());
        ++m_contentRevision;
        m_contentKey = std::move(key);
    }
    result.contentRevision = m_contentRevision;
    if (!result.coverageComplete) { result.emitters.clear(); result.deltaLights.clear(); result.shapes.clear(); }
    return result;
}

void RayPathSceneBuilder::Reset()
{
    m_geometries.clear();
    m_contentKey.clear();
}

const char* DescribeRayPathSceneIssue(RayPathSceneIssue issue)
{
    switch (issue) {
    case RayPathSceneIssue::RAY_SCENE_COVERAGE_INCOMPLETE: return "Path geometry / opacity coverage incomplete";
    case RayPathSceneIssue::SURFACE_UNSUPPORTED: return "Path surface unsupported";
    case RayPathSceneIssue::LIGHTING_UNSUPPORTED: return "Path light / environment unsupported";
    case RayPathSceneIssue::INVALID_LIGHTING: return "Path light radiance / direction invalid";
    case RayPathSceneIssue::INVALID_INSTANCE: return "Path dense instance / transform invalid";
    case RayPathSceneIssue::EMITTER_DOUBLE_SIDED_UNSUPPORTED: return "Double-sided Path emitter unsupported";
    case RayPathSceneIssue::BUFFER_READER_UNSUPPORTED: return "Path emitter CPU snapshot unavailable";
    case RayPathSceneIssue::BUFFER_CONTENT_MISMATCH: return "Path emitter buffer content version mismatch";
    case RayPathSceneIssue::INVALID_EMITTER_GEOMETRY: return "Path emitter range / index / triangle invalid";
    case RayPathSceneIssue::INVALID_EMISSION: return "Path emitter radiance invalid";
    case RayPathSceneIssue::UNREPRESENTABLE_SELECTION_PDF: return "Path emitter probability not representable";
    case RayPathSceneIssue::UNSUPPORTED_LIGHT_SOURCE: return "Path cookie / particle / day-night light unsupported";
    case RayPathSceneIssue::INVALID_LIGHT_SOURCE: return "Path owner / layer / light unit / shape invalid";
    case RayPathSceneIssue::AREA_MESH_BINDING_UNSUPPORTED: return "Path Area proxy is not a proven two-triangle rectangle";
    case RayPathSceneIssue::SHAPE_MESH_BINDING_UNSUPPORTED: return "Path Sphere / Tube owner mesh binding is ambiguous";
    }
    return "Unknown Path scene issue";
}

} /// @note namespace fbzz::renderer
