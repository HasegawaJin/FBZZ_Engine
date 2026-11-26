/// @file    MaterialPreview.hpp
/// @brief   Material アセット用プレビューウィジェット。
/// @author  Hasegawa Jin
/// @date    2026-08-19
#pragma once
#include <Editor/Panels/Preview/MaterialPreviewCore.hpp>
#include <Editor/Material/MaterialPreviewCache.hpp>
#include <Editor/Util/RevisionCache.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>

#include <imgui.h>

#include <cstdint>
#include <string>

namespace fbzz::asset { struct MaterialAssetAuthoring; }

namespace fbzz::editor {

struct EditorContext;
class OperatorRegistry;

/// @brief マテリアルプレビューの背景表示。
enum class MaterialPreviewBackground {
    Gradient, ///< AssetBrowser のサムネイル枠と同じ縦グラデーション (既定)
    Checker,  ///< 市松。アルファを持つ材質の «抜け» を見る
    Solid,
    Grid,     ///< 方眼。輪郭の歪みとタイリングを見る
};

/// @brief パネル単位で状態を持つマテリアルプレビュー。
/// @note Inspector と Preview パネルの視点操作は独立する。
class MaterialPreviewView {
public:
    /// @brief .mat を選択中の形状へ適用して描画する。
    /// @param[in] previewHeight キャンバスの最大高。0 ならツールバー描画後の残り高を使う。
    bool Draw(EditorContext& ctx, asset::MaterialAssetAuthoring material, float previewHeight);

    /// @brief 視点・照明・表示チャンネルを既定へ戻す。
    void ResetView();

    /// @brief このビューの表示設定を操作登録簿へ公開する。
    void RegisterOperators(OperatorRegistry& registry, std::string_view prefix);

private:
    void DrawToolbar(matpreview::Flavor flavor, bool gpuReady);
    void DrawBackground(ImVec2 min, ImVec2 max) const;
    /// @brief .mat の標準 normal スロットを raw RGB で表示する。
    void DrawNormalMap(EditorContext& ctx, const asset::MaterialAssetAuthoring& material);
    void DrawUnsupported(EditorContext& ctx, const asset::MaterialAssetAuthoring& material,
                         ImVec2 origin, float size);
    void SynchronizeResources(EditorContext& ctx);
    /// @note resources が null なら旧所有者のハンドルを忘れ、現在の資源プールへ返さない。
    void InvalidateGpu(renderer::ResourceManager* resources);
    bool EnsureGpuData(EditorContext& ctx, const asset::MaterialAssetAuthoring& material);
    bool EnsurePreviewFrame(EditorContext& ctx, const asset::MaterialAssetAuthoring& material,
                            matpreview::Flavor flavor);
    bool RenderFrame(EditorContext& ctx, matpreview::RenderDesc desc);

    matpreview::GpuData  m_gpu; ///< ソース材質のシェーダー、定数バッファ、テクスチャの描画キャッシュ。
    matpreview::Orbit    m_orbit; ///< プレビューの被写体を回り込むカメラの位置と向き。
    matpreview::Rig      m_rig; ///< 材質を照らすプレビュー専用のライトリグ設定。
    matpreview::Shape    m_shape   = matpreview::Shape::Sphere; ///< プレビューで材質を貼る形状。
    matpreview::Channel  m_channel = matpreview::Channel::Shaded; ///< プレビューで表示する材質のチャンネル。
    bool m_showParallax = true; ///< プレビューでパララックスの視差表現を描くか。

    renderer::ResourceHandle<renderer::RenderTargetTag> m_renderTarget; ///< このプレビュー画像の描画先として確保したレンダーターゲット。

    matpreview::PreviewResourceKey m_resourceKey; ///< デバイス、プロジェクト、アセット等の更新で GPU キャッシュを失効させるキー。
    asset::MaterialAssetAuthoring m_sourceMaterial; ///< 現在の表示対象として保持する元マテリアルのオーサリング値。
    RevisionCache<matpreview::PreviewGpuKey> m_gpuCache; ///< 最新の材質データから GPU プレビュー資源を再構築したかを判定するキャッシュ。
    RevisionCache<matpreview::PreviewFrameKey> m_frameCache; ///< 前回生成したレンダーターゲット画像を再利用できるか判定するキャッシュ。
    std::uint64_t m_sourceRevision = 0; ///< 表示対象のマテリアルが変わるたびに進むリビジョン。

    MaterialPreviewBackground m_background = MaterialPreviewBackground::Gradient; ///< プレビューで使う背景の種類。
    float m_solidColor[3] = { 0.13f, 0.14f, 0.17f }; ///< 単色背景を使う場合の RGB。

    bool m_animateWater = true; ///< Water / Fiber の停止操作は Trail / PostProcess の時刻にも適用する。

    /// @name Fiber
    /// @note Fiber の表示方式は .mat に保存せず、プレビュー状態で切り替える。
    /// @{
    matpreview::FiberMode m_fiberMode = matpreview::FiberMode::Shell; ///< プレビューで繊維を描く方式。
    int   m_fiberShellCount = 16; ///< Shell 方式で毛並みを描く重ね合わせの層数。
    float m_fiberWind = 0.0f; ///< 毛並みのプレビューに加える風の速さ [m/s]。
    /// @}

    renderer::ResourceHandle<renderer::TextureTag> m_fallbackTexture; ///< 3D プレビュー未対応 Flavor の代表テクスチャ。
    std::string m_fallbackTexturePath; ///< 標準の材質描画ができないときに代わりに表示する代表画像。
    bool        m_fallbackTextureResolved = false; ///< 現在の材質に対し代表画像のパス解決を試したか。

    renderer::ResourceHandle<renderer::TextureTag> m_normalMapTexture; ///< Flavor を問わず確認する .mat の raw normal map。
    std::string m_normalMapTexturePath; ///< 法線プレビューのために解決したテクスチャ原本パス。
    bool        m_normalMapTextureResolved = false; ///< 現在の材質の法線テクスチャのパスを解決済みか。
    bool        m_normalMapFileExists = false; ///< 前回の確認で法線テクスチャファイルが存在したか。
    std::int64_t m_normalMapFileRevision = 0; ///< 前回確認した法線テクスチャのファイル版。
    double      m_normalMapNextFileCheck = 0.0; ///< 法線テクスチャを次に確認する ImGui 時刻 [s]。
};

} // namespace fbzz::editor
