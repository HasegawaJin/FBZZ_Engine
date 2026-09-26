/// @file    MaterialPreview.hpp
/// @brief   Material アセット用プレビューウィジェット。
/// @author  Hasegawa Jin
/// @date    2026-08-19
#pragma once
#include <Editor/Panels/MaterialPreviewCore.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>

#include <imgui.h>

#include <cstdint>
#include <string>

namespace fbzz::asset { struct MaterialAsset; }

namespace fbzz::editor {

struct EditorContext;

/// @brief マテリアルプレビューの背景表示。
enum class MaterialPreviewBackground {
    Gradient, ///< AssetBrowser のサムネイル枠と同じ縦グラデーション (既定)
    Checker,  ///< 市松。アルファを持つ材質の «抜け» を見る
    Solid,    ///< 単色
    Grid,     ///< 方眼。輪郭の歪みとタイリングを見る
};

/// @brief パネル単位で状態を持つマテリアルプレビュー。
/// @note Inspector と Preview パネルの視点操作は独立する。
class MaterialPreviewView {
public:
    /// @brief .mat を選択中の形状へ適用して描画する。
    bool Draw(EditorContext& ctx, const asset::MaterialAsset& material, float previewHeight);

    /// @brief 視点・照明・表示チャンネルを既定へ戻す。
    void ResetView();

private:
    void DrawToolbar(EditorContext& ctx, matpreview::Flavor flavor);
    void DrawBackground(ImVec2 origin, float size, bool hovered) const;
    /// @brief .mat の標準 normal スロットを raw RGB で表示する。
    void DrawNormalMap(EditorContext& ctx, const asset::MaterialAsset& material);
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

    /// @brief Water と Fiber のプレビューを時間で進める。
    bool m_animateWater = true;

    /// @name Fiber
    /// @note Fiber の表示方式は .mat に保存せず、プレビュー状態で切り替える。
    /// @{
    matpreview::FiberMode m_fiberMode = matpreview::FiberMode::Shell;
    int   m_fiberShellCount = 16;
    float m_fiberWind = 0.0f;
    /// @}

    /// @brief 3D プレビュー未対応 Flavor の代表テクスチャ。
    renderer::ResourceHandle<renderer::TextureTag> m_fallbackTexture;
    std::string m_fallbackTexturePath;
    bool        m_fallbackTextureResolved = false;

    /// @brief Flavor を問わず確認する .mat の raw normal map。
    renderer::ResourceHandle<renderer::TextureTag> m_normalMapTexture;
    std::string m_normalMapTexturePath;
    bool        m_normalMapTextureResolved = false;
    bool        m_normalMapFileExists = false;
    std::int64_t m_normalMapFileRevision = 0;
    std::uint64_t m_normalMapResourceResetVersion = 0;
    double      m_normalMapNextFileCheck = 0.0;
};

} /// @note namespace fbzz::editor
