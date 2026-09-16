/// @file    VolumeFlipbookComparePreview.hpp
/// @brief   焼いた Flipbook を «MV なし | MV あり» の横並びで再生するプレビュー。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// WHY: この機能の価値は «MV でコマ間の補間が滑らかになる» ことで、それはマテリアルへ
///      適用してシーンで再生するまで確かめられなかった。焼いた直後の Atlas をそのまま GPU に
///      載せ、ゲームと同じ warp で左右に並べて再生する。
#pragma once

#include <Engine/Asset/VolumeFlipbookBaker.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>

#include <cstdint>
#include <string>

namespace fbzz::renderer { class IRenderer; class ResourceManager; }

namespace fbzz::editor {

class VolumeFlipbookComparePreview {
public:
    /// 焼き上がった Atlas を GPU へ載せる。前の Atlas は手放す。
    [[nodiscard]] bool Upload(renderer::ResourceManager& resources, asset::BakedVolumeFlipbook baked,
                              std::string& outError);
    /// time [秒] を playbackFps で再生した位置を描く。strengthScale は MV の強さの倍率 (1 = 規約どおり)。
    void Render(renderer::IRenderer& renderer, renderer::ResourceManager& resources, float time,
                float playbackFps, float strengthScale, asset::VolumePreviewBackground background);
    void Release(renderer::ResourceManager& resources);

    [[nodiscard]] bool HasFlipbook() const { return m_colorAtlas.IsValid(); }
    /// 2·tile × tile。左半分が MV なし、右半分が MV あり。
    [[nodiscard]] renderer::ResourceHandle<renderer::RenderTargetTag> Target() const { return m_target; }
    [[nodiscard]] int FrameCount() const { return m_grid.frameCount; }
    [[nodiscard]] float FrameDt() const { return m_frameDt; }
    [[nodiscard]] bool Loops() const { return m_loop; }
    [[nodiscard]] float MotionStrength() const { return m_motionStrength; }
    [[nodiscard]] int CurrentFrame() const { return m_currentFrame; }
    [[nodiscard]] float CurrentBlend() const { return m_currentBlend; }

private:
    [[nodiscard]] bool EnsureGpu(renderer::ResourceManager& resources, std::string& outError);

    renderer::ResourceHandle<renderer::ShaderTag> m_shader;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_pipeline;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_constants;
    renderer::ResourceHandle<renderer::TextureTag> m_colorAtlas;
    renderer::ResourceHandle<renderer::TextureTag> m_motionAtlas;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_target;
    std::uint64_t m_resetVersion = 0;

    asset::FlipbookGrid m_grid;
    std::uint32_t m_displayTile = 0;
    float m_motionStrength = 0.0f;
    float m_frameDt = 1.0f / 24.0f;
    bool  m_loop = false;
    int   m_currentFrame = 0;
    float m_currentBlend = 0.0f;
};

} // namespace fbzz::editor
