/// @file    RayEnvironment.hpp
/// @brief   Raw HDR cube の線形 CPU snapshot と立体角 importance 分布。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <Math/Vector3.hpp>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::renderer {
class Camera;
class ResourceManager;
struct RenderEnvironmentInput;

/// @note +X,-X,+Y,-Y,+Z,-Z の raw mip0 由来の線形 RGB。CPU importance は face 128 以下へ縮小し、GPU 放射輝度は原 mip0 を読む。
/// @note contentVersion=0 は未公開版。公開した immutable 内容を変えたら非0の版を進め、畳み込み IBL を渡さない。
struct RayEnvironmentPixels {
    uint32_t faceSize = 0;
    uint64_t contentVersion = 0;
    std::vector<std::array<float, 3>> radiance;
};
struct RayEnvironmentInput {
    ResourceHandle<TextureTag> rawTexture;
    std::shared_ptr<const RayEnvironmentPixels> pixels;
    float intensity = 1;
    /// @note cube の +X を world の -Z へ回す右手系 Y 回転 [rad]。
    float rotationRadians = 0;
    bool requested = false;
    bool ready = false;
};
/// @note t7 の16 bytes。selectionPdf は丸め後 CDF の実区間、方向 PDF は UV-to-solid-angle Jacobian を掛ける。
struct RayEnvironmentRecord {
    float selectionCdf = 0;
    float selectionPdf = 0;
    float solidAngle = 0;
    uint32_t reserved = 0;
};
static_assert(sizeof(RayEnvironmentRecord) == 16);
struct RayEnvironmentDistribution {
    uint32_t faceSize = 0;
    uint64_t contentVersion = 0;
    std::vector<RayEnvironmentRecord> records;
};

/// @return Explicit black sky or finite nonnegative camera RGB is known; otherwise false and out is unchanged.
/// @note Raw HDR requests, emitting sky/disks/cloud and an unfilled DepthOnly camera require another environment provider. Background alpha is not radiance.
[[nodiscard]] bool ResolveRayConstantEnvironment(const RenderEnvironmentInput& environment,
    const Camera& camera, math::Vector3& out);

/// @return 不正な HDR / 未完成なら false、out は未変更。
/// @note 5% の solid-angle mixture は黒い texel にも正の PDF を与え、bilinear raw lookup の台を被覆する。
/// @see https://pbr-book.org/4ed/Light_Sources/Infinite_Area_Lights Image importance distributions
[[nodiscard]] bool BuildRayEnvironmentDistribution(const RayEnvironmentInput& input,
    RayEnvironmentDistribution& out);
[[nodiscard]] double RayCubeTexelSolidAngle(uint32_t x, uint32_t y, uint32_t faceSize);
[[nodiscard]] math::Vector3 RayCubeDirection(uint32_t face, float u, float v);
[[nodiscard]] float RayEnvironmentDirectionPdf(const RayEnvironmentDistribution& distribution,
    const math::Vector3& direction, float rotationRadians = 0);

/// @note Main-thread。公開済み texture revision ごとに CPU 読取を更新し、immutable snapshot をビュー間で共有する。
/// @note GPU の所有は ResourceManager/asset lease に残す。GPU mip0 と同じ raw DDS だけを受理する。
class RayEnvironmentCache {
public:
    [[nodiscard]] std::shared_ptr<const RayEnvironmentPixels> Load(const std::string& sourcePath,
        ResourceHandle<TextureTag> texture, uint64_t publishedRevision, ResourceManager& resources);
    void Reset();
private:
    struct Entry {
        std::string path;
        ResourceHandle<TextureTag> texture;
        uint64_t publishedRevision = 0;
        uint64_t textureContentVersion = 0;
        std::shared_ptr<const RayEnvironmentPixels> pixels;
    };
    std::vector<Entry> m_entries;
    const ResourceManager* m_owner = nullptr;
    uint64_t m_resetVersion = 0;
    uint64_t m_revision = 0;
};
} /// @note namespace fbzz::renderer
