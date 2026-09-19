/// @file    MaterialPreview.hpp
/// @brief   Material アセット用プレビューウィジェット。
/// @author  Hasegawa Jin
/// @date    2026-08-19
#pragma once
#include <Editor/Panels/MaterialPreviewCore.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>

#include <imgui.h>

#include <string>

namespace fbzz::asset { struct MaterialAsset; }

namespace fbzz::editor {

struct EditorContext;

/// プレビュー背景の描き方。
enum class MaterialPreviewBackground {
    Gradient, ///< AssetBrowser のサムネイル枠と同じ縦グラデーション (既定)
    Checker,  ///< 市松。アルファを持つ材質の «抜け» を見る
    Solid,    ///< 単色
    Grid,     ///< 方眼。輪郭の歪みとタイリングを見る
};

/// パネル 1 つぶんのプレビュー状態と描画。
/// @note 以前は static 1 個を Inspector と Preview パネルが共有しており、片方でズームや回転を
///       すると、もう片方の見え方まで同時に変わっていた。2 つ並べて比べる用途ができなかったため、
///       インスタンスとして持たせる。
class MaterialPreviewView {
public:
    /// .mat を選択中の形状へ適用して描画する。焼けたら true。
    bool Draw(EditorContext& ctx, const asset::MaterialAsset& material, float previewHeight);

    /// 視点・照明・表示チャンネルを既定へ戻す (形状と背景は保つ)。
    void ResetView();

private:
    void DrawToolbar(EditorContext& ctx, matpreview::Flavor flavor);
    void DrawBackground(ImVec2 origin, float size, bool hovered) const;
    void DrawUnsupported(EditorContext& ctx, const asset::MaterialAsset& material,
                         ImVec2 origin, float size);
    bool RenderFrame(EditorContext& ctx, const asset::MaterialAsset& material,
                     matpreview::Flavor flavor);

    matpreview::GpuData  m_gpu;
    matpreview::Orbit    m_orbit;
    matpreview::Rig      m_rig;
    matpreview::Shape    m_shape   = matpreview::Shape::Sphere;
    matpreview::Channel  m_channel = matpreview::Channel::Shaded;

    renderer::ResourceHandle<renderer::RenderTargetTag> m_renderTarget;

    MaterialPreviewBackground m_background = MaterialPreviewBackground::Gradient;
    float m_solidColor[3] = { 0.13f, 0.14f, 0.17f };

    /// Water の波・Fiber の突風を進めるか。止めると波形を静止させて法線マップを読める。
    bool m_animateWater = true;

    /// @name Fiber
    /// @note .mat は描画方式を持たないため、プレビュー側で選んで見比べる。層数と風は FiberComponent の既定から始める。
    /// @{
    matpreview::FiberMode m_fiberMode = matpreview::FiberMode::Shell;
    int   m_fiberShellCount = 24;
    float m_fiberWind = 0.0f;
    /// @}

    /// 3D へ焼けない .mat (Particle / Trail / Decal …) の代表テクスチャ。
    renderer::ResourceHandle<renderer::TextureTag> m_fallbackTexture;
    std::string m_fallbackTexturePath;
    bool        m_fallbackTextureResolved = false;
};

} // namespace fbzz::editor
