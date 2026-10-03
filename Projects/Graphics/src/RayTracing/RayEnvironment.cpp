/// @file    RayEnvironment.cpp
/// @brief   Raw HDR DDS の読み取りと cube texel の importance PDF。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <Graphics/RayTracing/RayEnvironment.hpp>
#include <Graphics/Renderer/Camera.hpp>
#include <Graphics/Renderer/RenderEnvironment.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/AssetPathService.hpp>
#define WIN32_LEAN_AND_MEAN
#include <DirectXTex.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace fbzz::renderer {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kMaxFaceSize = 128;
double SolidAnglePrimitive(double x, double y)
{
    return std::atan2(x * y, std::sqrt(x * x + y * y + 1));
}
bool DecodeRawCube(const std::string& source, RayEnvironmentPixels& output)
{
    DirectX::TexMetadata metadata;
    DirectX::ScratchImage image;
    const auto path = std::filesystem::u8path(source).wstring();
    if (FAILED(DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image))
        || !metadata.IsCubemap() || metadata.arraySize != 6 || metadata.width != metadata.height
        || metadata.width == 0 || DirectX::IsSRGB(metadata.format)) return false;
    const bool hdr = metadata.format == DXGI_FORMAT_R16G16B16A16_FLOAT
        || metadata.format == DXGI_FORMAT_R32G32B32A32_FLOAT
        || metadata.format == DXGI_FORMAT_R11G11B10_FLOAT
        || metadata.format == DXGI_FORMAT_BC6H_UF16 || metadata.format == DXGI_FORMAT_BC6H_SF16;
    if (!hdr) return false;
    RayEnvironmentPixels decoded;
    decoded.faceSize = static_cast<uint32_t>(std::min<size_t>(metadata.width, kMaxFaceSize));
    decoded.radiance.resize(static_cast<size_t>(decoded.faceSize) * decoded.faceSize * 6);
    for (size_t face = 0; face < 6; ++face) {
        const DirectX::Image* pixels = image.GetImage(0, face, 0);
        if (!pixels) return false;
        DirectX::ScratchImage converted;
        if (DirectX::IsCompressed(pixels->format)) {
            if (FAILED(DirectX::Decompress(*pixels, DXGI_FORMAT_R32G32B32A32_FLOAT, converted))) return false;
            pixels = converted.GetImage(0, 0, 0);
        } else if (pixels->format != DXGI_FORMAT_R32G32B32A32_FLOAT) {
            if (FAILED(DirectX::Convert(*pixels, DXGI_FORMAT_R32G32B32A32_FLOAT,
                DirectX::TEX_FILTER_DEFAULT, 0, converted))) return false;
            pixels = converted.GetImage(0, 0, 0);
        }
        /// @note GPU は原 mip0 を読むため、縮小で負値や非有限値が相殺される前に全 texel を検証する。
        for (size_t y = 0; y < pixels->height; ++y)
            for (size_t x = 0; x < pixels->width; ++x) {
                std::array<float, 4> value{};
                std::memcpy(value.data(), pixels->pixels + y * pixels->rowPitch + x * sizeof(value), sizeof(value));
                for (size_t component = 0; component < 3; ++component)
                    if (!std::isfinite(value[component]) || value[component] < 0) return false;
            }
        DirectX::ScratchImage reduced;
        if (pixels->width != decoded.faceSize) {
            if (FAILED(DirectX::Resize(*pixels, decoded.faceSize, decoded.faceSize,
                DirectX::TEX_FILTER_BOX, reduced))) return false;
            pixels = reduced.GetImage(0, 0, 0);
        }
        for (uint32_t y = 0; y < decoded.faceSize; ++y)
            for (uint32_t x = 0; x < decoded.faceSize; ++x) {
                std::array<float, 4> value{};
                std::memcpy(value.data(), pixels->pixels + y * pixels->rowPitch + x * sizeof(value), sizeof(value));
                for (size_t component = 0; component < 3; ++component)
                    if (!std::isfinite(value[component]) || value[component] < 0) return false;
                decoded.radiance[(face * decoded.faceSize + y) * decoded.faceSize + x]
                    = {value[0], value[1], value[2]};
            }
    }
    output = std::move(decoded);
    return true;
}
bool CubeCoordinates(math::Vector3 d, uint32_t& face, double& u, double& v)
{
    const double x = std::abs(d.x), y = std::abs(d.y), z = std::abs(d.z);
    if (!std::isfinite(x + y + z) || std::max({x, y, z}) == 0) return false;
    if (x >= y && x >= z) {
        face = d.x >= 0 ? 0 : 1;
        u = (d.x >= 0 ? -d.z : d.z) / x; v = -d.y / x;
    } else if (y >= z) {
        face = d.y >= 0 ? 2 : 3;
        u = d.x / y; v = (d.y >= 0 ? d.z : -d.z) / y;
    } else {
        face = d.z >= 0 ? 4 : 5;
        u = (d.z >= 0 ? d.x : -d.x) / z; v = -d.y / z;
    }
    return true;
}
} /// @note namespace

bool ResolveRayConstantEnvironment(const RenderEnvironmentInput& environment,
    const Camera& camera, math::Vector3& out)
{
    if (environment.rayEnvironment.requested || environment.cloudEnabled) return false;
    const bool blackSky = environment.sky && environment.sky->skyScatterIntensity == 0.0f;
    if (environment.sky && !blackSky) return false;
    if (environment.sunMoon
        && ((environment.sunMoon->sunEnabled && environment.sunMoon->sunDiskIntensity != 0.0f)
            || (environment.sunMoon->moonEnabled && environment.sunMoon->moonBrightness != 0.0f))) return false;
    if (!blackSky && camera.m_clearMode != CameraClearMode::SolidColor) return false;
    const auto& background = camera.m_backgroundColor;
    if (!std::isfinite(background.x) || !std::isfinite(background.y) || !std::isfinite(background.z)
        || background.x < 0 || background.y < 0 || background.z < 0) return false;
    out = blackSky ? math::Vector3::ZERO : math::Vector3{background.x, background.y, background.z};
    return true;
}

double RayCubeTexelSolidAngle(uint32_t x, uint32_t y, uint32_t size)
{
    if (!size || x >= size || y >= size) return 0;
    const double x0 = 2.0 * x / size - 1, x1 = 2.0 * (x + 1) / size - 1;
    const double y0 = 2.0 * y / size - 1, y1 = 2.0 * (y + 1) / size - 1;
    /// @note cube projection の Jacobian (1+u^2+v^2)^(-3/2) をセル境界で積分する。
    /// @see https://www.pbr-book.org/4ed/Light_Sources/Infinite_Area_Lights Directional PDF and solid angle measure.
    return SolidAnglePrimitive(x1, y1) - SolidAnglePrimitive(x0, y1)
        - SolidAnglePrimitive(x1, y0) + SolidAnglePrimitive(x0, y0);
}
math::Vector3 RayCubeDirection(uint32_t face, float u, float v)
{
    math::Vector3 direction;
    switch (face) {
    case 0: direction = {1, -v, -u}; break;
    case 1: direction = {-1, -v, u}; break;
    case 2: direction = {u, 1, v}; break;
    case 3: direction = {u, -1, -v}; break;
    case 4: direction = {u, -v, 1}; break;
    default: direction = {-u, -v, -1}; break;
    }
    return direction.Normalized();
}
bool BuildRayEnvironmentDistribution(const RayEnvironmentInput& input, RayEnvironmentDistribution& out)
{
    if (!input.requested || !input.ready || !input.pixels || !std::isfinite(input.intensity)
        || input.intensity < 0 || !std::isfinite(input.rotationRadians)) return false;
    const auto& pixels = *input.pixels;
    const size_t count = static_cast<size_t>(pixels.faceSize) * pixels.faceSize * 6;
    if (!pixels.contentVersion || !pixels.faceSize || pixels.faceSize > kMaxFaceSize || pixels.radiance.size() != count) return false;
    std::vector<double> weights(count), solidAngles(count);
    double total = 0;
    for (size_t index = 0; index < count; ++index) {
        const auto& value = pixels.radiance[index];
        for (float component : value) if (!std::isfinite(component) || component < 0) return false;
        const uint32_t x = static_cast<uint32_t>(index % pixels.faceSize);
        const uint32_t y = static_cast<uint32_t>(index / pixels.faceSize % pixels.faceSize);
        const double angle = RayCubeTexelSolidAngle(x, y, pixels.faceSize);
        solidAngles[index] = angle;
        weights[index] = (0.2126 * value[0] + 0.7152 * value[1] + 0.0722 * value[2]) * angle;
        total += weights[index];
    }
    if (!std::isfinite(total)) return false;
    RayEnvironmentDistribution result;
    result.faceSize = pixels.faceSize;
    result.contentVersion = pixels.contentVersion;
    result.records.resize(count);
    double accumulated = 0;
    float previous = 0;
    for (size_t index = 0; index < count; ++index) {
        const double uniform = solidAngles[index] / (4 * kPi);
        const double probability = total > 0 ? 0.95 * weights[index] / total + 0.05 * uniform : uniform;
        accumulated += probability;
        const float cdf = index + 1 == count ? 1.0f : static_cast<float>(accumulated);
        if (!std::isfinite(cdf) || cdf <= previous || cdf > 1) return false;
        result.records[index] = {cdf, cdf - previous, static_cast<float>(solidAngles[index]), 0};
        previous = cdf;
    }
    out = std::move(result);
    return true;
}
float RayEnvironmentDirectionPdf(const RayEnvironmentDistribution& distribution,
    const math::Vector3& direction, float rotation)
{
    if (!std::isfinite(rotation) || !distribution.faceSize) return 0;
    const float c = std::cos(rotation), s = std::sin(rotation);
    const math::Vector3 local{c * direction.x - s * direction.z, direction.y,
        s * direction.x + c * direction.z};
    uint32_t face;
    double u, v;
    if (!CubeCoordinates(local, face, u, v)) return 0;
    const uint32_t size = distribution.faceSize;
    const auto x = std::min(size - 1, static_cast<uint32_t>(std::max(0.0, (u + 1) * size * 0.5)));
    const auto y = std::min(size - 1, static_cast<uint32_t>(std::max(0.0, (v + 1) * size * 0.5)));
    const size_t index = (static_cast<size_t>(face) * size + y) * size + x;
    if (index >= distribution.records.size()) return 0;
    return static_cast<float>(distribution.records[index].selectionPdf * size * size * 0.25
        * std::pow(1 + u * u + v * v, 1.5));
}
std::shared_ptr<const RayEnvironmentPixels> RayEnvironmentCache::Load(const std::string& reference,
    ResourceHandle<TextureTag> texture, uint64_t revision, ResourceManager& resources)
{
    if (m_owner != &resources || m_resetVersion != resources.GetResetVersion()) {
        Reset();
        m_owner = &resources;
        m_resetVersion = resources.GetResetVersion();
    }
    const auto* gpu = resources.Get(texture);
    if (!revision || !gpu || !gpu->IsRayEnvironmentTexture()) return {};
    const auto textureVersion = gpu->GetContentVersion();
    std::string source;
    if (!ResolveTextureSource(reference, source)) source = reference;
    source = ResolveAssetPath(source);
    for (const auto& entry : m_entries)
        if (entry.path == source && entry.texture == texture && entry.publishedRevision == revision
            && entry.textureContentVersion == textureVersion) return entry.pixels;
    RayEnvironmentPixels decoded;
    std::shared_ptr<const RayEnvironmentPixels> snapshot;
    if (DecodeRawCube(source, decoded)) {
        decoded.contentVersion = ++m_revision;
        snapshot = std::make_shared<const RayEnvironmentPixels>(std::move(decoded));
    }
    std::erase_if(m_entries, [&](const auto& entry) { return entry.path == source; });
    if (m_entries.size() >= 8) m_entries.erase(m_entries.begin());
    m_entries.push_back({source, texture, revision, textureVersion, snapshot});
    return snapshot;
}
void RayEnvironmentCache::Reset()
{
    /// @note revision は保持し、同じ cache の再使用で履歴版 ABA を作らない。
    m_entries.clear();
}
} /// @note namespace fbzz::renderer
