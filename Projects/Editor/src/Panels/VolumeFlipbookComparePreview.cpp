/// @file    VolumeFlipbookComparePreview.cpp
/// @brief   焼いた Flipbook の «MV なし | MV あり» 比較再生の実装。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include "VolumeFlipbookComparePreview.hpp"

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/Format.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::editor {
namespace {

constexpr const char* kCompareShaderPath = "Assets/Shaders/Bake/VolumeFlipbook/FlipbookCompare.hlsl";
/// 表示用なので、大きいタイルは縮めて描く (4096 のタイルを 2 枚並べる必要は無い)。
constexpr std::uint32_t kMaxDisplayTile = 512;

/// FlipbookCompare.hlsl の cbuffer と 1:1。
struct alignas(16) CompareConstants {
    float currentRect[4];
    float nextRect[4];
    float blend;
    float strength;
    std::uint32_t tileSize;
    std::uint32_t background;
};
static_assert(sizeof(CompareConstants) == 48, "FlipbookCompare.hlsl の cbuffer と一致させること");

/// ParticlePass の SpriteRectForFrame と同じ並び (左上から行優先)。
void StoreTileRect(float (&out)[4], int frame, const asset::FlipbookGrid& grid)
{
    const int columns = (std::max)(grid.columns, 1);
    const int rows = (std::max)(grid.rows, 1);
    out[0] = static_cast<float>(frame % columns) / static_cast<float>(columns);
    out[1] = static_cast<float>(frame / columns) / static_cast<float>(rows);
    out[2] = 1.0f / static_cast<float>(columns);
    out[3] = 1.0f / static_cast<float>(rows);
}

} // namespace

bool VolumeFlipbookComparePreview::EnsureGpu(renderer::ResourceManager& resources, std::string& outError)
{
    if (m_resetVersion != resources.GetResetVersion()) {
        m_shader = {};
        m_pipeline = {};
        m_constants = {};
        m_target = {};
        m_colorAtlas = {};
        m_motionAtlas = {};
        m_resetVersion = resources.GetResetVersion();
    }
    if (!m_shader.IsValid())
        m_shader = resources.LoadShader(asset::AssetManager::ResolveAssetPath(kCompareShaderPath));
    if (!m_shader.IsValid()) {
        outError = "比較プレビューのシェーダーを読み込めません";
        return false;
    }
    if (!m_pipeline.IsValid()) {
        m_pipeline = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL,
                                                     renderer::BlendMode::OPAQUE_BLEND,
                                                     renderer::DepthMode::DEPTH_OFF });
    }
    if (!m_constants.IsValid())
        m_constants = resources.CreateConstantBuffer(sizeof(CompareConstants));
    return true;
}

bool VolumeFlipbookComparePreview::Upload(renderer::ResourceManager& resources, asset::BakedVolumeFlipbook baked,
                                          std::string& outError)
{
    if (baked.colorRgba8.empty() || baked.motionRgba8.empty() || baked.atlasWidth == 0 || baked.atlasHeight == 0) {
        outError = "焼き上がった Atlas がありません";
        return false;
    }
    Release(resources);
    if (!EnsureGpu(resources, outError)) return false;
    m_colorAtlas = resources.CreateTexture(baked.colorRgba8.data(), baked.atlasWidth, baked.atlasHeight);
    m_motionAtlas = resources.CreateTexture(baked.motionRgba8.data(), baked.atlasWidth, baked.atlasHeight);
    m_displayTile = (std::min)(baked.tileSize, kMaxDisplayTile);
    m_target = resources.CreateRenderTarget(m_displayTile * 2, m_displayTile,
                                            renderer::RenderTargetDesc{ 1, renderer::Format::RGBA8, false });
    if (!m_colorAtlas.IsValid() || !m_motionAtlas.IsValid() || !m_target.IsValid()) {
        Release(resources);
        outError = "比較プレビュー用のテクスチャを作れません";
        return false;
    }
    m_grid = baked.grid;
    m_motionStrength = baked.motionStrength;
    m_frameDt = baked.frameDt;
    m_loop = baked.loop;
    m_currentFrame = 0;
    m_currentBlend = 0.0f;
    return true;
}

void VolumeFlipbookComparePreview::Render(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                                          float time, float playbackFps, float strengthScale,
                                          asset::VolumePreviewBackground background)
{
    std::string error;
    if (!HasFlipbook() || !EnsureGpu(resources, error)) return;
    /// @note デバイスリセットで Atlas も消えた。再ベイクまで描かない。
    if (!m_colorAtlas.IsValid()) return;

    const int frames = (std::max)(m_grid.frameCount, 1);
    const float position = (std::max)(time, 0.0f) * (std::max)(playbackFps, 0.0f);
    float framePosition = m_loop ? std::fmod(position, static_cast<float>(frames))
                                 : (std::min)(position, static_cast<float>(frames - 1));
    m_currentFrame = std::clamp(static_cast<int>(std::floor(framePosition)), 0, frames - 1);
    m_currentBlend = framePosition - static_cast<float>(m_currentFrame);
    /// @note ParticlePass と同じく、ループしない列の最後は自分自身へ向かって補間する (= 補間なし)。
    const int nextFrame = m_loop ? (m_currentFrame + 1) % frames : (std::min)(m_currentFrame + 1, frames - 1);

    CompareConstants constants{};
    StoreTileRect(constants.currentRect, m_currentFrame, m_grid);
    StoreTileRect(constants.nextRect, nextFrame, m_grid);
    constants.blend = m_currentBlend;
    constants.strength = m_motionStrength * (std::max)(strengthScale, 0.0f);
    constants.tileSize = m_displayTile;
    constants.background = static_cast<std::uint32_t>(background);
    resources.Update(m_constants, &constants, sizeof(constants));

    renderer.SetRenderTarget(m_target, resources);
    renderer.Clear({ 0.0f, 0.0f, 0.0f, 1.0f });
    renderer::DrawCall draw;
    draw.shader = m_shader;
    draw.pipelineState = m_pipeline;
    draw.vertexCount = 3;
    draw.constantBuffers[0] = m_constants;
    draw.textures[0] = m_colorAtlas;
    draw.textures[1] = m_motionAtlas;
    renderer.Submit(draw, resources);
    /// @note DX12 は RT を切り替えたときにシェーダー読み取り状態へ戻す。ImGui 表示はそれを前提にしている。
    renderer.SetRenderTarget({}, resources);
}

void VolumeFlipbookComparePreview::Release(renderer::ResourceManager& resources)
{
    if (m_resetVersion == resources.GetResetVersion()) {
        if (m_colorAtlas.IsValid()) resources.Release(m_colorAtlas);
        if (m_motionAtlas.IsValid()) resources.Release(m_motionAtlas);
        if (m_target.IsValid()) resources.Release(m_target);
    }
    m_colorAtlas = {};
    m_motionAtlas = {};
    m_target = {};
}

} // namespace fbzz::editor
