/// @file    WaterfallFluidClip.cpp
/// @brief   Bake and stream quantized surfaces from a local liquid simulation.
/// @author  Hasegawa Jin
/// @date    2026-11-11
#include <Engine/Asset/WaterfallFluidClip.hpp>
#include <Engine/Asset/ChunkFile.hpp>
#include <Fluid/FluidSolver.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace fbzz::asset {
namespace {
using math::Vector3;
constexpr auto CONTENT_KIND = MakeFourCC('W', 'F', 'L', '1');
constexpr int MAX_FRAMES = 120;
constexpr std::size_t MAX_VERTICES = 65535;
constexpr std::size_t MAX_INDICES = 360000;
constexpr std::size_t MAX_CLIP_BYTES = 64 * 1024 * 1024;

struct ClipInfo {
    std::uint32_t frameCount;
    std::uint32_t framesPerSecond;
};

struct FrameInfo {
    std::uint32_t vertices;
    std::uint32_t indices;
};

struct PackedVertex {
    std::uint16_t position[3];
    std::int8_t normal[3];
    std::uint8_t coverage;
};
static_assert(sizeof(PackedVertex) == 10);

std::uint16_t QuantizePosition(float value)
{
    return static_cast<std::uint16_t>(std::lround(std::clamp(value * 0.5f + 0.5f, 0.0f, 1.0f) * 65535.0f));
}

std::int8_t QuantizeNormal(float value)
{
    return static_cast<std::int8_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 127.0f));
}

std::size_t Index(int n, int x, int y, int z)
{
    return static_cast<std::size_t>(x + n * (y + n * z));
}

float TerrainHeight(const WaterfallFluidBakeInput& input, float x, float z)
{
    if (input.ground.empty()) return -2.0f;
    const float scale = 0.5f * static_cast<float>(input.grid - 1);
    const float u = std::clamp(x + 1.0f, 0.0f, 2.0f) * scale;
    const float v = std::clamp(z + 1.0f, 0.0f, 2.0f) * scale;
    const int ix = (std::min)(static_cast<int>(u), input.grid - 2);
    const int iz = (std::min)(static_cast<int>(v), input.grid - 2);
    const auto h = [&](int sx, int sz) { return input.ground[Index(input.grid, sx, sz, 0)]; };
    return std::lerp(std::lerp(h(ix, iz), h(ix + 1, iz), u - ix),
        std::lerp(h(ix, iz + 1), h(ix + 1, iz + 1), u - ix), v - iz);
}

float ObstacleDistance(const WaterfallFluidObstacle& obstacle, const Vector3& point)
{
    const Vector3 offset = point - obstacle.center;
    if (obstacle.sphereRadius > 0.0f) return offset.Length() - obstacle.sphereRadius;
    const Vector3 local = obstacle.rotation.Conjugate() * offset;
    const Vector3 q{std::abs(local.x) - obstacle.halfSize.x, std::abs(local.y) - obstacle.halfSize.y,
        std::abs(local.z) - obstacle.halfSize.z};
    const Vector3 outside{(std::max)(q.x, 0.0f), (std::max)(q.y, 0.0f), (std::max)(q.z, 0.0f)};
    return outside.Length() + (std::min)((std::max)({q.x, q.y, q.z}), 0.0f);
}

bool SampleContact(const void* context, const Vector3& point, float radius, fluid::FluidLiquidContact& out)
{
    const auto& input = *static_cast<const WaterfallFluidBakeInput*>(context);
    const float pool = point.y - input.receiverHeight;
    float terrain = point.y - TerrainHeight(input, point.x, point.z);
    constexpr float delta = 0.003f;
    out.distance = pool;
    out.restitution = 0.23f;
    if (terrain < radius * 3.0f) {
        const float dx = TerrainHeight(input, point.x + delta, point.z) - TerrainHeight(input, point.x - delta, point.z);
        const float dz = TerrainHeight(input, point.x, point.z + delta) - TerrainHeight(input, point.x, point.z - delta);
        const auto normal = Vector3{-dx, 2.0f * delta, -dz}.NormalizedOr(Vector3::UP);
        terrain *= normal.y;
        if (terrain < out.distance) out = {normal, terrain, 0.12f};
    }
    for (const auto& obstacle : input.obstacles) {
        const float distance = ObstacleDistance(obstacle, point);
        if (distance >= radius * 1.05f || distance >= out.distance) continue;
        const Vector3 dx{delta, 0.0f, 0.0f}, dy{0.0f, delta, 0.0f}, dz{0.0f, 0.0f, delta};
        const Vector3 gradient{ObstacleDistance(obstacle, point + dx) - ObstacleDistance(obstacle, point - dx),
            ObstacleDistance(obstacle, point + dy) - ObstacleDistance(obstacle, point - dy),
            ObstacleDistance(obstacle, point + dz) - ObstacleDistance(obstacle, point - dz)};
        out = {gradient.NormalizedOr(Vector3::UP), distance, 0.18f};
    }
    return out.distance < radius * 1.05f;
}

bool Drain(const void* context, const Vector3& point, const Vector3& velocity)
{
    const auto& input = *static_cast<const WaterfallFluidBakeInput*>(context);
    /// @note Let incoming particles reach the impact plane before absorbing settled water.
    return std::abs(point.x) > 1.0f || std::abs(point.z) > 1.0f || point.y > 1.0f
        || (point.y < input.receiverHeight + 0.06f && std::abs(velocity.y) < 0.09f);
}

void Splat(const fluid::FluidLiquidSolver& solver, const WaterfallFluidBakeInput& input, std::vector<float>& density)
{
    const int n = input.grid;
    const float scale = 0.5f * static_cast<float>(n - 1);
    const float radius = (std::max)(1.25f, solver.ParticleRadius() * scale * 2.1f);
    density.assign(Index(n, 0, 0, n), 0.0f);
    for (const auto& particle : solver.Particles()) {
        const Vector3 center{(particle.x + 1.0f) * scale, (particle.y + 1.0f) * scale, (particle.z + 1.0f) * scale};
        const auto first = [&](float value) { return (std::max)(1, static_cast<int>(std::floor(value - radius))); };
        const auto last = [&](float value) { return (std::min)(n - 2, static_cast<int>(std::ceil(value + radius))); };
        for (int z = first(center.z); z <= last(center.z); ++z)
            for (int y = first(center.y); y <= last(center.y); ++y)
                for (int x = first(center.x); x <= last(center.x); ++x) {
                    const float q = 1.0f - (Vector3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)}
                        - center).LengthSq() / (radius * radius);
                    if (q > 0.0f) density[Index(n, x, y, z)] += q * q * q;
                }
    }
}

/// @brief Surface nets use one vertex per active cell and a shared quad around each crossed edge.
/// @see https://www.merl.com/publications/TR99-24 Frisken Gibson, Constrained Elastic SurfaceNets (cell connectivity).
bool ExtractSurface(const WaterfallFluidBakeInput& input, const std::vector<float>& density,
    std::vector<int>& cells, std::vector<PackedVertex>& vertices, std::vector<std::uint16_t>& indices)
{
    constexpr float iso = 0.32f;
    const int n = input.grid;
    const float spacing = 2.0f / static_cast<float>(n - 1);
    cells.assign(Index(n, 0, 0, n), -1);
    vertices.clear();
    indices.clear();
    for (int z = 0; z < n - 1; ++z)
        for (int y = 0; y < n - 1; ++y)
            for (int x = 0; x < n - 1; ++x) {
                std::array<float, 8> samples{};
                unsigned int inside = 0;
                for (unsigned int corner = 0; corner < samples.size(); ++corner) {
                    samples[corner] = density[Index(n, x + (corner & 1), y + ((corner >> 1) & 1),
                        z + (corner >> 2))];
                    if (samples[corner] >= iso) inside |= 1u << corner;
                }
                if (inside == 0 || inside == 255) continue;
                if (vertices.size() >= MAX_VERTICES) return false;

                const auto offset = [](unsigned int corner) {
                    return Vector3{static_cast<float>(corner & 1), static_cast<float>((corner >> 1) & 1),
                        static_cast<float>(corner >> 2)};
                };
                Vector3 average{}, gradient{};
                int crossings = 0;
                for (unsigned int axis = 0; axis < 3; ++axis) {
                    const unsigned int bit = 1u << axis;
                    for (unsigned int low = 0; low < samples.size(); ++low) {
                        if (low & bit) continue;
                        const auto high = low | bit;
                        const float difference = samples[low] - samples[high];
                        gradient += offset(bit) * difference;
                        if (((inside >> low) ^ (inside >> high)) & 1u) {
                            average += Vector3::Lerp(offset(low), offset(high), (samples[low] - iso) / difference);
                            ++crossings;
                        }
                    }
                }
                const Vector3 p = (Vector3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)}
                    + average / static_cast<float>(crossings)) * spacing - Vector3::ONE;
                const auto normal = gradient.NormalizedOr(Vector3::UP);
                const float wall = 1.0f - (std::max)({std::abs(p.x), std::abs(p.z), p.y});
                const float top = input.inlet.y - p.y;
                const float bottom = p.y - input.receiverHeight;
                const float coverage = std::clamp((std::min)({wall * 8.0f, top * 6.0f, bottom * 18.0f}), 0.0f, 1.0f);
                const auto cell = Index(n, x, y, z);
                cells[cell] = static_cast<int>(vertices.size());
                vertices.push_back({{QuantizePosition(p.x), QuantizePosition(p.y), QuantizePosition(p.z)},
                    {QuantizeNormal(normal.x), QuantizeNormal(normal.y), QuantizeNormal(normal.z)},
                    static_cast<std::uint8_t>(std::lround(coverage * 255.0f))});

                const std::array strides{1, n, n * n};
                const std::array coords{x, y, z};
                for (unsigned int axis = 0; axis < 3; ++axis) {
                    const auto u = (axis + 1) % 3, v = (axis + 2) % 3;
                    if (coords[u] == 0 || coords[v] == 0 || ((inside ^ (inside >> (1u << axis))) & 1u) == 0) continue;
                    const int a = cells[cell], b = cells[cell - strides[u]];
                    const int c = cells[cell - strides[u] - strides[v]], d = cells[cell - strides[v]];
                    if ((std::min)({b, c, d}) < 0) continue;
                    if (indices.size() + 6 > MAX_INDICES) return false;
                    for (int index : (inside & 1u) ? std::array{a, b, c, a, c, d} : std::array{a, d, c, a, c, b})
                        indices.push_back(static_cast<std::uint16_t>(index));
                }
            }
    return true;
}

fluid::FluidRecipe MakeRecipe(const WaterfallFluidBakeInput& input, float duration)
{
    fluid::FluidRecipe recipe;
    recipe.kind = fluid::FluidKind::Liquid;
    recipe.liquid.gravity = input.gravity;
    recipe.liquid.floor = false;
    recipe.liquid.particleLifetime = 1.25f;
    recipe.liquid.maxParticles = (std::min)(6000, input.particlesPerSecond * 2);
    recipe.liquid.particleRadius = 0.90f / static_cast<float>(input.grid);
    recipe.liquid.solverIterations = 3;
    recipe.liquid.cohesion = 0.18f;
    const int sources = std::clamp(static_cast<int>(std::ceil(input.inletHalfWidth * 15.0f)), 1, 8);
    for (int i = 0; i < sources; ++i) {
        auto& source = recipe.sources.emplace_back();
        const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(sources);
        source.center = input.inlet + input.inletAxis * (input.inletHalfWidth * (2.0f * u - 1.0f));
        source.size = Vector3::ONE * (std::max)(recipe.liquid.particleRadius, input.inletHalfWidth * 0.52f / sources);
        source.velocity = input.inletVelocity;
        source.duration = duration;
        source.count = static_cast<int>(std::ceil(duration * input.particlesPerSecond / sources));
        source.spread = 0.10f;
    }
    return recipe;
}
} /// @note namespace

std::string BakeWaterfallFluidClip(const WaterfallFluidBakeInput& input, const std::string& path,
    const std::atomic<bool>& cancel, std::atomic<int>& progress)
{
    const auto finite = [](const Vector3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    if (input.grid < 24 || input.grid > 64 || input.frameCount < 8 || input.frameCount > MAX_FRAMES
        || input.framesPerSecond < 12 || input.framesPerSecond > 30
        || input.particlesPerSecond < 100 || input.particlesPerSecond > 3000
        || !finite(input.inlet) || !finite(input.inletVelocity) || !finite(input.inletAxis)
        || !std::isfinite(input.gravity) || input.gravity <= 0.0f || !std::isfinite(input.receiverHeight)
        || !std::isfinite(input.inletHalfWidth) || input.inletHalfWidth <= 0.0f || input.inletHalfWidth > 1.0f
        || (!input.ground.empty() && input.ground.size() != static_cast<std::size_t>(input.grid * input.grid)))
        return "Invalid waterfall fluid bake settings.";

    constexpr int warmupFrames = 24;
    const float dt = 1.0f / static_cast<float>(input.framesPerSecond);
    const auto recipe = MakeRecipe(input, (input.frameCount + warmupFrames + 1) * dt);
    fluid::FluidLiquidSolver solver;
    solver.Reset(recipe, true);
    const fluid::FluidLiquidBoundary boundary{&input, SampleContact, Drain};
    ChunkFileWriter writer;
    const ClipInfo clip{static_cast<std::uint32_t>(input.frameCount), static_cast<std::uint32_t>(input.framesPerSecond)};
    std::vector<std::uint8_t> header(sizeof(clip));
    std::memcpy(header.data(), &clip, sizeof(clip));
    writer.AddChunk(0, std::move(header));

    std::vector<float> density;
    std::vector<int> cells;
    std::vector<PackedVertex> vertices;
    std::vector<std::uint16_t> indices;
    std::size_t storedBytes = 0;
    bool hasSurface = false;
    for (int frame = -warmupFrames; frame < input.frameCount; ++frame) {
        if (cancel.load()) return "Bake canceled.";
        solver.Advance(dt, boundary);
        progress.store((frame + warmupFrames + 1) * 99 / (input.frameCount + warmupFrames));
        if (frame < 0) continue;
        Splat(solver, input, density);
        if (!ExtractSurface(input, density, cells, vertices, indices))
            return "Fluid mesh budget exceeded. Reduce grid resolution or emission.";
        const FrameInfo info{static_cast<std::uint32_t>(vertices.size()), static_cast<std::uint32_t>(indices.size())};
        const std::size_t vertexBytes = vertices.size() * sizeof(PackedVertex);
        const std::size_t indexBytes = indices.size() * sizeof(std::uint16_t);
        std::vector<std::uint8_t> bytes(sizeof(info) + vertexBytes + indexBytes);
        if (storedBytes + bytes.size() > MAX_CLIP_BYTES) return "Fluid clip exceeds 64 MiB. Reduce the frame count or resolution.";
        storedBytes += bytes.size();
        std::memcpy(bytes.data(), &info, sizeof(info));
        if (vertexBytes) std::memcpy(bytes.data() + sizeof(info), vertices.data(), vertexBytes);
        if (indexBytes) std::memcpy(bytes.data() + sizeof(info) + vertexBytes, indices.data(), indexBytes);
        hasSurface |= !indices.empty();
        writer.AddChunk(static_cast<std::uint32_t>(frame + 1), std::move(bytes));
    }
    if (cancel.load()) return "Bake canceled.";
    if (!hasSurface) return "No visible liquid surface. Check the inlet, rocks, and emission.";
    if (!writer.WriteAtomically(path, CONTENT_KIND, input.stamp)) return "Could not write the waterfall fluid clip.";
    progress.store(100);
    return {};
}

bool LoadWaterfallFluidFrame(const std::string& path, std::uint64_t stamp, int requestedFrame, WaterfallFluidFrame& out)
{
    ChunkFileReader reader;
    std::vector<std::uint8_t> bytes;
    ClipInfo clip{};
    std::error_code error;
    const auto fileBytes = std::filesystem::file_size(util::FileSystem::PathFromUtf8(path), error);
    if (requestedFrame < 0 || error || fileBytes > MAX_CLIP_BYTES + 8192
        || !reader.Open(path) || reader.Header().contentKind != CONTENT_KIND
        || reader.Header().sourceStamp != stamp || reader.Header().chunkCount > MAX_FRAMES + 1) return false;
    const auto* header = reader.Find(0);
    if (!header || header->rawSize != sizeof(clip) || !reader.ReadChunk(0, bytes)) return false;
    std::memcpy(&clip, bytes.data(), sizeof(clip));
    if (clip.frameCount < 8 || clip.frameCount > MAX_FRAMES || clip.framesPerSecond < 12 || clip.framesPerSecond > 30
        || reader.Header().chunkCount != clip.frameCount + 1) return false;
    const auto frame = static_cast<std::uint32_t>(requestedFrame) % clip.frameCount;
    const auto* chunk = reader.Find(frame + 1);
    const std::size_t maxBytes = sizeof(FrameInfo) + MAX_VERTICES * sizeof(PackedVertex) + MAX_INDICES * sizeof(std::uint16_t);
    if (!chunk || chunk->rawSize > maxBytes || !reader.ReadChunk(frame + 1, bytes) || bytes.size() < sizeof(FrameInfo))
        return false;
    FrameInfo info{};
    std::memcpy(&info, bytes.data(), sizeof(info));
    const std::size_t vertexBytes = static_cast<std::size_t>(info.vertices) * sizeof(PackedVertex);
    const std::size_t indexBytes = static_cast<std::size_t>(info.indices) * sizeof(std::uint16_t);
    if (info.vertices > MAX_VERTICES || info.indices > MAX_INDICES || info.indices % 3 != 0
        || bytes.size() != sizeof(info) + vertexBytes + indexBytes) return false;

    WaterfallFluidFrame candidate;
    candidate.frame = static_cast<int>(frame);
    candidate.frameCount = static_cast<int>(clip.frameCount);
    candidate.framesPerSecond = static_cast<int>(clip.framesPerSecond);
    candidate.vertices.reserve(info.vertices);
    for (std::uint32_t i = 0; i < info.vertices; ++i) {
        PackedVertex vertex{};
        std::memcpy(&vertex, bytes.data() + sizeof(info) + i * sizeof(vertex), sizeof(vertex));
        candidate.vertices.push_back({
            Vector3{static_cast<float>(vertex.position[0]), static_cast<float>(vertex.position[1]),
                static_cast<float>(vertex.position[2])} * (2.0f / 65535.0f) - Vector3::ONE,
            Vector3{static_cast<float>(vertex.normal[0]), static_cast<float>(vertex.normal[1]),
                static_cast<float>(vertex.normal[2])}.NormalizedOr(Vector3::UP), vertex.coverage / 255.0f});
    }
    candidate.indices.resize(info.indices);
    if (indexBytes) std::memcpy(candidate.indices.data(), bytes.data() + sizeof(info) + vertexBytes, indexBytes);
    if (std::any_of(candidate.indices.begin(), candidate.indices.end(), [&](auto index) { return index >= info.vertices; })) return false;
    out = std::move(candidate);
    return true;
}
} /// @note namespace fbzz::asset
