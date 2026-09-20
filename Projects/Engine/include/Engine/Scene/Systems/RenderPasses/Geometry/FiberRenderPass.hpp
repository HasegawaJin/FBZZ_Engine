/// @file    FiberRenderPass.hpp
/// @brief   静的繊維の色・GBuffer・影・速度を共有ジオメトリから描く。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Engine/Scene/Systems/RenderPasses/IRenderPass.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <array>
#include <cstdint>
#include <string>

namespace fbzz::math { struct Frustum; }
namespace fbzz::asset { struct FiberMaterialSettings; struct MaterialAsset; }
namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

/// @brief 直前に読んだ fiberMask。同じパスの読込失敗を毎フレーム繰り返さないために持つ。
struct FiberMaskSlot {
    std::string m_path;
    renderer::ResourceHandle<renderer::TextureTag> m_texture;
};

/// @brief .mat の fiberMask (textures の tex5) を読み、settings.m_maskIndex へ bindless 添字を書く。
/// @note 未設定・読込失敗・解放済みは白 1×1 の添字にする (マスク無しと同じ見た目)。シェーダーは添字を常に有効として引く。
/// @note 添字はテクスチャの生存に紐づくので、CB へ上げる前に毎フレーム呼ぶ。
void ResolveFiberMaskTexture(asset::FiberMaterialSettings& settings, const asset::MaterialAsset* material,
    renderer::ResourceManager& resources, FiberMaskSlot& slot);
/// @brief パスを直接渡す版。FiberComponent::m_maskPath の個体ごとの差し替えに使う。空パスは白 1×1。
void ResolveFiberMaskTexture(asset::FiberMaterialSettings& settings, const std::string& path,
    renderer::ResourceManager& resources, FiberMaskSlot& slot);

struct PerFrameCB;

/// @brief 繊維の b5。風・時刻・層数・LOD と局所 FlowField の範囲。
/// @note LAYOUT: Assets/Shaders/Fiber/FiberCommon.hlsli の FiberFrameConstants と一致させる。
/// @note 風はワールド [m/s]、時刻 [s]。lod.x は Blade の提出率、lod.y は根元ディザの残存率 (1 で全部残す)。
struct FiberFrameCB {
    math::Vector3 m_wind;
    float m_time = 0.0f;
    float m_shellCount = 1.0f;
    float m_hybrid = 0.0f;
    float m_turbulence = 0.0f;
    float m_pulseFrequency = 0.0f;
    math::Vector4 m_lod{1,1,0,0};
    /// @note 今フレームは [0, count)、前フレームは [previousFirst, +previousCount)。
    uint32_t m_flowCount = 0;
    uint32_t m_flowPreviousFirst = 0;
    uint32_t m_flowPreviousCount = 0;
    uint32_t m_flowChannels = 0;
};

/// @brief 繊維の b10。足元の接触 32 件 × (現在, 前フレーム)。
/// @note LAYOUT: FiberCommon.hlsli の FiberContactConstants と一致させる。radius (centers.w) が 0 の枠は無効。
struct FiberContactCB {
    std::array<math::Vector4,64> m_centers{};
    std::array<math::Vector4,64> m_times{};
    /// @note シェーダーが走査する枠数 [0,32]。今フレームは発生済みで回復しきっていない最後の枠 + 1。
    uint32_t m_count = 0;
    /// @note 前フレームは評価時刻が履歴ごとに違うため時刻で絞らず、半径を持つ最後の枠 + 1。
    uint32_t m_previousCount = 0;
    uint32_t m_padding[2]{};
};

/// @brief シェーダーが走査する接触の枠数を詰め直す。
/// @param time 今フレームの評価時刻 [s]。FiberFrameCB::m_time と同じ値を渡す。
/// @note 今フレームは «発生済み (age >= 0) かつ回復しきっていない (age < recovery)» 最後の枠 + 1。途中の無効枠はシェーダー側で飛ばす。
/// @note 前フレームは速度履歴ごとに評価時刻が 1〜2 フレーム前へずれるため、時刻では絞らず半径を持つ最後の枠 + 1。
inline void UpdateFiberContactCounts(FiberContactCB& data, float time)
{
    data.m_count = 0;
    data.m_previousCount = 0;
    for (uint32_t i = 0; i < 32; ++i) {
        const float age = time - data.m_times[i].x;
        const float recovery = data.m_times[i].z < 0.05f ? 0.05f : data.m_times[i].z;
        if (data.m_centers[i].w > 0.0f && age >= 0.0f && age < recovery) data.m_count = i + 1;
        if (data.m_centers[i + 32].w > 0.0f) data.m_previousCount = i + 1;
    }
}

/// @note Forward は不透明と半透明が同じパスなので、その間から同じ描画関数を呼ぶ。
/// @see Docs/design/fiber-rendering.md
void ExecuteFiberPass(RenderPassContext& ctx);
void ExecuteFiberGBufferPass(RenderPassContext& ctx);
void ExecuteFiberVelocityPass(RenderPassContext& ctx);
void ExecuteFiberSelectionMask(RenderPassContext& ctx);
/// @note 呼び出し元の深度 RT とアトラス viewport を維持する。光源のビューで Fin の輪郭を評価する。
void SubmitFiberShadowCasters(RenderPassContext& ctx, const PerFrameCB& lightFrame,
                             const math::Frustum& lightFrustum);

class FiberRenderPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "FiberForward"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources&, RenderPassContext& ctx) override { ExecuteFiberPass(ctx); }
};

} // namespace fbzz::scene

static_assert(sizeof(fbzz::scene::FiberFrameCB) == 64);
static_assert(sizeof(fbzz::scene::FiberContactCB) == 2064);
