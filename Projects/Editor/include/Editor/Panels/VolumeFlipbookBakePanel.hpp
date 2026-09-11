/// @file    VolumeFlipbookBakePanel.hpp
/// @brief   解析ボリュームを Flipbook + Motion Vector アトラスへ焼くエディターパネル。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// Tools > "Volume Flipbook Baker..." から開く。View > Panels には表示しない。
/// タブは 2 つ: «Volume» (焼く前のボリュームを再生) と «Flipbook» (焼いた結果を MV なし/ありで比べる)。
/// GPU の駆動は OnBeforeBegin で行う。
/// WHY OnRenderContent ではないか: ドッキングしたタブが裏に回ると OnRenderContent は呼ばれず、
///     ベイクが途中で止まる。OnBeforeBegin は visible である限り毎フレーム、フレーム内で呼ばれる。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Engine/Asset/VolumeFlipbookBaker.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::editor {

class VolumeFlipbookComparePreview;

class VolumeFlipbookBakePanel final : public IPanel {
public:
    VolumeFlipbookBakePanel();
    ~VolumeFlipbookBakePanel() override;

    const char* GetWindowName()        const override { return "Volume Flipbook Baker"; }
    const char* GetViewMenuName()      const override { return "Volume Flipbook Baker"; }
    bool        ShowInViewMenu()       const override { return false; }
    bool        GetDefaultVisibility() const override { return false; }
    void        OnInit(EditorContext& ctx) override;
    void        OnShutdown() override;

protected:
    void OnBeforeBegin(EditorContext& ctx) override;
    void OnRenderContent(EditorContext& ctx) override;
    /// ベイク中に閉じると OnBeforeBegin が呼ばれなくなり、途中で止まる。
    bool CanClose() const override { return !m_baker.IsBusy(); }

private:
    enum class VolumeView : std::uint8_t { Color, Alpha, Motion };

    void DrawSourceSettings();
    void DrawLookSettings();
    void DrawFramingWarnings();
    void DrawVolumeTab(EditorContext& ctx);
    void DrawFlipbookTab(EditorContext& ctx);
    void DrawDisplayControls();
    void DrawBake(EditorContext& ctx);
    void DrawApply(EditorContext& ctx);
    void StartBake(EditorContext& ctx);
    void ApplyToMaterial(EditorContext& ctx);
    void OpenInPrefabPreview(EditorContext& ctx);
    void MarkSettingsChanged();
    [[nodiscard]] float PreviewWidth(float aspect) const;
    [[nodiscard]] float BakeDuration() const;

    asset::VolumeFlipbookBaker        m_baker;
    asset::VolumeFlipbookBakeSettings m_settings;
    renderer::ResourceManager*        m_resources = nullptr;
    std::unique_ptr<VolumeFlipbookComparePreview> m_compare;
    asset::VolumeFramingReport        m_framing;
    bool                              m_framingDirty = true;

    // 表示 (両タブ共通)
    asset::VolumePreviewBackground m_background = asset::VolumePreviewBackground::Dark;
    int  m_previewSize = 1;   ///< 0 = 256 / 1 = 384 / 2 = 512 / 3 = 幅に合わせる
    bool m_selectFlipbookTab = false;

    // Volume タブ
    float      m_previewTime = 0.0f;
    bool       m_previewDirty = true;
    bool       m_playing = false;
    float      m_playSpeed = 1.0f;
    VolumeView m_view = VolumeView::Color;
    bool       m_showLightArrow = true;

    // Flipbook タブ
    float m_compareTime = 0.0f;
    bool  m_comparePlaying = true;
    /// 焼いた FPS より遅く再生すると、コマ間の補間の差がよく見える。
    float m_compareFps = 6.0f;
    float m_strengthScale = 1.0f;

    bool  m_resultHandled = true;
    std::array<char, 64> m_baseName = {};
    std::string m_materialPath;
    std::string m_status;
    bool        m_statusIsError = false;
    asset::VolumeFlipbookBakeResult m_lastResult;
};

} // namespace fbzz::editor
