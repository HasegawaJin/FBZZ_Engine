/// @file    RayEnvironment.hpp
/// @brief   Raw HDR cube の線形 CPU snapshot と立体角 importance 分布。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
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
    uint32_t faceSize = 0; ///< CPU スナップショットのキューブ 1 面の幅 [texel]。
    uint64_t contentVersion = 0; ///< 生の環境画像の公開内容版。0 は未公開。
    std::vector<std::array<float, 3>> radiance; ///< 6 面を順に並べたシーン線形 RGB 放射輝度の texel 列。
};
struct RayEnvironmentInput {
    ResourceHandle<TextureTag> rawTexture; ///< GPU が放射輝度を読む、畳み込み前の環境キューブの非所有ハンドル。
    std::shared_ptr<const RayEnvironmentPixels> pixels; ///< 同じ生キューブから作った不変 CPU 放射輝度の共有スナップショット。
    float intensity = 1; ///< 生の環境放射輝度に掛ける倍率。
    /// @note cube の +X を world の -Z へ回す右手系 Y 回転 [rad]。
    float rotationRadians = 0; ///< キューブの向きをワールド Y 軸まわりに回す角度 [rad]。
    bool requested = false; ///< このビューで生の環境キューブを要求したか。
    bool ready = false; ///< GPU キューブと対応する CPU 入力を積分に使えるか。
};
/// @note t7 の16 bytes。selectionPdf は丸め後 CDF の実区間、方向 PDF は UV-to-solid-angle Jacobian を掛ける。
struct RayEnvironmentRecord {
    float selectionCdf = 0; ///< この texel まで含めたサンプリングの累積確率。
    float selectionPdf = 0; ///< 丸め後の累積確率の差から求めた、この texel の離散選択確率。
    float solidAngle = 0; ///< texel がキューブ原点から覆う立体角 [sr]。
    uint32_t reserved = 0; ///< GPU レコードの配置を保つ予約領域。
};
static_assert(sizeof(RayEnvironmentRecord) == 16);
struct RayEnvironmentDistribution {
    uint32_t faceSize = 0; ///< このサンプリング表のキューブ 1 面の幅 [texel]。
    uint64_t contentVersion = 0; ///< 表の作成に使った環境画像の内容版。
    std::vector<RayEnvironmentRecord> records; ///< 環境方向のサンプリングに使うキューブ面順の texel レコード列。
};

/// @return Explicit black sky or finite nonnegative camera RGB is known; otherwise false and out is unchanged.
/// @note Raw HDR requests, emitting sky/disks/cloud and an unfilled DepthOnly camera require another environment provider. Background alpha is not radiance.
[[nodiscard]] FBZZ_GRAPHICS_API bool ResolveRayConstantEnvironment(const RenderEnvironmentInput& environment,
    const Camera& camera, math::Vector3& out);

/// @return 不正な HDR / 未完成なら false、out は未変更。
/// @note 5% の solid-angle mixture は黒い texel にも正の PDF を与え、bilinear raw lookup の台を被覆する。
/// @see https://pbr-book.org/4ed/Light_Sources/Infinite_Area_Lights Image importance distributions
[[nodiscard]] FBZZ_GRAPHICS_API bool BuildRayEnvironmentDistribution(const RayEnvironmentInput& input,
    RayEnvironmentDistribution& out);
[[nodiscard]] FBZZ_GRAPHICS_API double RayCubeTexelSolidAngle(uint32_t x, uint32_t y, uint32_t faceSize);
[[nodiscard]] FBZZ_GRAPHICS_API math::Vector3 RayCubeDirection(uint32_t face, float u, float v);
[[nodiscard]] FBZZ_GRAPHICS_API float RayEnvironmentDirectionPdf(const RayEnvironmentDistribution& distribution,
    const math::Vector3& direction, float rotationRadians = 0);

/// @note Main-thread。公開済み texture revision ごとに CPU 読取を更新し、immutable snapshot をビュー間で共有する。
/// @note GPU の所有は ResourceManager/asset lease に残す。GPU mip0 と同じ raw DDS だけを受理する。
class RayEnvironmentCache {
public:
    [[nodiscard]] FBZZ_GRAPHICS_API std::shared_ptr<const RayEnvironmentPixels> Load(const std::string& sourcePath,
        ResourceHandle<TextureTag> texture, uint64_t publishedRevision, ResourceManager& resources);
    FBZZ_GRAPHICS_API void Reset();
private:
    struct Entry {
        std::string path; ///< 生の環境キューブの読み出し元ファイルのパス。
        ResourceHandle<TextureTag> texture; ///< CPU スナップショットと照合する GPU テクスチャの非所有ハンドル。
        uint64_t publishedRevision = 0; ///< 読み出しに使用したアセット公開版。
        uint64_t textureContentVersion = 0; ///< GPU テクスチャの内容を公開アセットと照合する版。
        std::shared_ptr<const RayEnvironmentPixels> pixels; ///< この公開版から作った不変の CPU 放射輝度スナップショット。
    };
    std::vector<Entry> m_entries; ///< 環境キューブの公開版ごとに作った CPU スナップショットのキャッシュ。
    const ResourceManager* m_owner = nullptr; ///< GPU ハンドルの所属を照合する資源管理器の非所有参照。
    uint64_t m_resetVersion = 0; ///< 資源管理器がリセットされる前後を区別する版。
    uint64_t m_revision = 0; ///< CPU 放射輝度スナップショットに採番する内容版。
};
} // namespace fbzz::renderer
