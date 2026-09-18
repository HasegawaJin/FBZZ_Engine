/// @file    ScriptGraphicsProxy.hpp
/// @brief   Script から明るさと画質プリセットを操作する。
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// @note postprocess は PostProcessVolume が自由に差し替える「シーンの見た目」の面。
///       プレイヤー設定をそこへ置くとボリューム進入時に消えるため、別レイヤーに置く。
/// @note 触る先は実行中の RenderSettings。編集中 (FBZZ_EXECUTE_ALWAYS) から呼んでも
///       ProjectSettings.toml へ保存されてしまうため何も起きない。
/// @see Docs/design/game-settings.md
#pragma once

#include <Engine/Renderer/RenderSettings.hpp>
#include <cstdint>

namespace fbzz::scene {

class Script;

struct ScriptGraphicsProxy {
    Script* script = nullptr;

    /// @name 明るさ
    /// Composite が画面フェードの直前に 1 度だけ掛ける。1.0 で無加工。
    ///@{
    void  SetBrightness(float value) const;   ///< 0.1 - 4.0 にクランプされる
    [[nodiscard]] float GetBrightness() const;
    /// 発光の強さ。bloom.intensity へ掛ける倍率 (0 - 2)。0 で発光なし。
    /// @note SetBloom(bool) はパスごと切る画質設定、こちらは効き具合を選ぶ好みの設定。
    ///       Option では別の行に出る。
    void  SetBloomScale(float value) const;
    [[nodiscard]] float GetBloomScale() const;
    ///@}

    /// @name 描画スケール
    /// 内部の描画解像度だけを倍率で変える。出力は元の寸法のまま、UI も影響を受けない。
    /// GPU コストは面積比なので 0.7 でおよそ半分。
    ///@{
    void  SetRenderScale(float scale) const;  ///< 0.5 - 2.0 にクランプされる
    [[nodiscard]] float GetRenderScale() const;
    /// 現在の内部描画解像度 (倍率と下限を適用した後の実サイズ)。
    /// Option 画面に「1920x1080 → 1344x756」のように出す用。
    ///
    /// @note 基準はウィンドウの寸法。Editor の Play ではゲームビューポートが
    ///       ウィンドウより小さいため、表示上の数値が実際の内部 RT と一致しない
    ///       (配布ゲームでは一致する)。表示専用の値として扱うこと。
    [[nodiscard]] uint32_t GetRenderWidth() const;
    [[nodiscard]] uint32_t GetRenderHeight() const;
    ///@}

    /// @name 画質プリセット
    /// 影・AA・AO・反射・体積光をまとめて切り替える。適用後に個別 setter で上書きできる。
    ///@{
    void SetQualityPreset(renderer::QualityPreset preset) const;
    /// 現在の構成と一致するプリセット。個別調整後は一番近い (超えない) 段を返す。
    [[nodiscard]] renderer::QualityPreset GetQualityPreset() const;
    ///@}

    /// @name 個別調整
    /// FXAA/TAA と SSAO/GTAO は排他。両方立てようとすると高品質側が落ちる。
    ///@{
    void SetShadowsEnabled(bool enabled) const;
    [[nodiscard]] bool GetShadowsEnabled() const;
    /// シャドウアトラス全体の解像度 (512 - 8192)。2 の冪へ丸めはしない。
    void SetShadowResolution(uint32_t resolution) const;
    [[nodiscard]] uint32_t GetShadowResolution() const;
    /// カスケード分割数 (1 - 4)。
    void SetShadowCascades(int count) const;
    [[nodiscard]] int GetShadowCascades() const;

    void SetSSR(bool enabled) const;
    [[nodiscard]] bool GetSSR() const;
    void SetGTAO(bool enabled) const;
    [[nodiscard]] bool GetGTAO() const;
    void SetSSAO(bool enabled) const;
    [[nodiscard]] bool GetSSAO() const;
    void SetTAA(bool enabled) const;
    [[nodiscard]] bool GetTAA() const;
    void SetFXAA(bool enabled) const;
    [[nodiscard]] bool GetFXAA() const;
    void SetBloom(bool enabled) const;
    [[nodiscard]] bool GetBloom() const;
    void SetMotionBlur(bool enabled) const;
    [[nodiscard]] bool GetMotionBlur() const;
    void SetVolumetricLight(bool enabled) const;
    [[nodiscard]] bool GetVolumetricLight() const;
    void SetContactShadow(bool enabled) const;
    [[nodiscard]] bool GetContactShadow() const;
    ///@}
};

} // namespace fbzz::scene
