/// @file    VolumeFlipbookBakePanel.hpp
/// @brief   解析ボリュームを Flipbook + Motion Vector アトラスへ焼くエディターパネル。
/// @author  Hasegawa Jin
/// @date    2026-09-11
/// @note Tools > "Volume Flipbook Baker..." から開く。View > Panels には表示しない。
/// @note タブは 2 つ: «Volume» (焼く前のボリュームを再生) と «Flipbook» (焼いた結果を MV なし/ありで比べる)。
/// @note Baker は FluidBakeService の 1 つを共有する (160³ の GPU 資源を 2 つ抱えない)。焼きはサービスの
/// @note ジョブとして走るので、パネルを閉じても止まらない。プレビューだけは OnBeforeBegin で描く
/// @note (ドッキングしたタブが裏に回ると OnRenderContent は呼ばれないが、OnBeforeBegin はフレーム内で呼ばれる)。
/// @note 扱うのは解析ソース (puff) だけ。.fluid の Inspector と同じファイルへ別々に書き戻すと後勝ちが相手の編集を
/// @note       潰すため、.fluid のプレビューと焼きは Fluid Editor だけが持つ。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Engine/Asset/Flipbook/Volume/VolumeFlipbookBaker.hpp>

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
    void OnRegisterOperators(OperatorRegistry& registry) override;
    void OnRenderFrame(EditorContext& ctx) override;

protected:
    void OnBeforeBegin(EditorContext& ctx) override;
    void OnRenderContent(EditorContext& ctx) override;

private:
    enum class VolumeView : std::uint8_t { Color, Alpha, Motion, SixWayPositive, SixWayNegative };

    void DrawSourceSettings(EditorContext& ctx);
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
    /// @note 自分が出したジョブが終わっていれば結果を受け取る。
    void HandleFinishedJob(EditorContext& ctx);
    /// @note 自分のジョブが待っている / 走っている。
    [[nodiscard]] bool JobActive(const EditorContext& ctx) const;
    [[nodiscard]] float PreviewWidth(float aspect) const;
    [[nodiscard]] float BakeDuration() const;

    asset::VolumeFlipbookBakeSettings m_settings; ///< 現在編集中のボリュームフリップブックのベイク設定。
    renderer::ResourceManager*        m_resources = nullptr; ///< プレビュー資源を確保・解放するために借りる ResourceManager。
    std::unique_ptr<VolumeFlipbookComparePreview> m_compare; ///< 元のボリュームとベイク画像を比較描画する所有プレビュー。
    asset::VolumeFramingReport        m_framing; ///< ベイク対象を収めるカメラのフレーミング計算結果。
    bool                              m_framingDirty = true; ///< 入力変更後にフレーミングを再計算する必要があるか。

    asset::VolumePreviewBackground m_background = asset::VolumePreviewBackground::Dark; ///< 表示 (両タブ共通)
    int  m_previewSize = 1;   ///< 0 = 256 / 1 = 384 / 2 = 512 / 3 = 幅に合わせる
    bool m_selectFlipbookTab = false; ///< 次の描画でベイク済みフリップブックのタブを選択する要求。

    float      m_previewTime = 0.0f; ///< Volume タブ
    bool       m_previewDirty = true; ///< 現在の設定を反映するためにプレビューを描き直す必要があるか。
    bool       m_playing = false; ///< ベイク設定側のボリュームプレビューを再生中か。
    float      m_playSpeed = 1.0f; ///< ボリュームプレビューの時間の進み方に掛ける倍率。
    VolumeView m_view = VolumeView::Color; ///< ボリュームプレビューに表示するチャンネルやビューの種類。
    bool       m_showLightArrow = true; ///< ボリュームプレビューに照明方向の矢印を表示するか。

    float m_compareTime = 0.0f; ///< Flipbook タブ
    bool  m_comparePlaying = true; ///< 結果比較ビューでフリップブックを再生中か。
    float m_compareFps = 6.0f; ///< 焼いた FPS より遅く再生すると、コマ間の補間の差がよく見える。
    float m_strengthScale = 1.0f; ///< 結果比較ビューでモーションベクトルの補間に掛ける強度倍率。

    std::uint32_t m_jobId = 0; ///< 自分が FluidBakeService へ出した焼きのジョブ (0 = 無し)。
    std::uint32_t m_apiJobId = 0; ///< Editor API から要求されたベイクジョブの識別番号。要求がなければ 0。
    bool m_apiPreview = false; ///< パネルが閉じている間も Editor API のプレビュー要求を進めるか。
    std::array<char, 64> m_baseName = {}; ///< ベイク成果物のファイル名に使う拡張子なしの名前。
    std::string m_materialPath; ///< ベイク結果を適用するマテリアルアセットのパス。
    std::string m_status; ///< 現在のベイクとプレビューに関する UI 向けの結果メッセージ。
    bool        m_statusIsError = false; ///< m_status を失敗メッセージとして表示するか。
    asset::VolumeFlipbookBakeResult   m_lastResult; ///< このパネルで直近に完了したボリュームフリップブックのベイク結果。
    asset::VolumeFlipbookBakeSettings m_lastBakeSettings; ///< m_lastResult を焼いたときの設定 (マテリアルのループ判定と FPS に使う)。
};

} // namespace fbzz::editor
