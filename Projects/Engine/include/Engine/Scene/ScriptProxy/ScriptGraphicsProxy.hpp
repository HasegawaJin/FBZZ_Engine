/// @file    ScriptGraphicsProxy.hpp
/// @brief   Script から明るさと画質プリセットを操作する。
/// @author  Hasegawa Jin
/// @date    2026-08-23

/// @note postprocess は PostProcessVolume が自由に差し替える「シーンの見た目」の面。
/// @note プレイヤー設定をそこへ置くとボリューム進入時に消えるため、別レイヤーに置く。
/// @note 触る先は実行中の RenderSettings。編集中 (ExecuteAlways) から呼んでも
/// @note ProjectSettings.toml へ保存されてしまうため何も起きない。
/// @see Docs/design/game-settings.md
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Renderer/RenderSettings.hpp>
#include <cstdint>

namespace fbzz::scene {

class Script;

struct ScriptGraphicsProxy {
    Script* script = nullptr;

    /// @name 明るさ
    /// @note Composite が画面フェードの直前に 1 度だけ掛ける。1.0 で無加工。
    /// @{
    FBZZ_ENGINE_API void  SetBrightness(float value) const;   ///< @note 0.1 - 4.0 にクランプされる
    [[nodiscard]] FBZZ_ENGINE_API float GetBrightness() const;
    /// @note 発光の強さ。bloom.intensity へ掛ける倍率 (0 - 2)。0 で発光なし。
    /// @note SetBloom(bool) はパスごと切る画質設定、こちらは効き具合を選ぶ好みの設定。
    /// @note Option では別の行に出る。
    FBZZ_ENGINE_API void  SetBloomScale(float value) const;
    [[nodiscard]] FBZZ_ENGINE_API float GetBloomScale() const;
    /// @}

    /// @name 描画スケール
    /// @note 内部の描画解像度だけを倍率で変える。出力は元の寸法のまま、UI も影響を受けない。
    /// @note GPU コストは面積比なので 0.7 でおよそ半分。
    /// @{
    FBZZ_ENGINE_API void  SetRenderScale(float scale) const;  ///< @note 0.5 - 2.0 にクランプされる
    [[nodiscard]] FBZZ_ENGINE_API float GetRenderScale() const;
    /// @note 現在の内部描画解像度 (倍率と下限を適用した後の実サイズ)。
    /// @note Option 画面に「1920x1080 → 1344x756」のように出す用。

    /// @note 基準はウィンドウの寸法。Editor の Play ではゲームビューポートが
    /// @note ウィンドウより小さいため、表示上の数値が実際の内部 RT と一致しない
    /// @note (配布ゲームでは一致する)。表示専用の値として扱うこと。
    [[nodiscard]] FBZZ_ENGINE_API uint32_t GetRenderWidth() const;
    [[nodiscard]] FBZZ_ENGINE_API uint32_t GetRenderHeight() const;
    /// @}

    /// @name 画質プリセット
    /// @note 影・AA・AO・反射・体積光をまとめて切り替える。適用後に個別 setter で上書きできる。
    /// @{
    FBZZ_ENGINE_API void SetQualityPreset(renderer::QualityPreset preset) const;
    /// @note 現在の構成と一致するプリセット。個別調整後は一番近い (超えない) 段を返す。
    [[nodiscard]] FBZZ_ENGINE_API renderer::QualityPreset GetQualityPreset() const;
    /// @}

    /// @name 個別調整
    /// @note FXAA/TAA と SSAO/GTAO は排他。両方立てようとすると高品質側が落ちる。
    /// @{
    FBZZ_ENGINE_API void SetShadowsEnabled(bool enabled) const;
    [[nodiscard]] FBZZ_ENGINE_API bool GetShadowsEnabled() const;
    /// @note シャドウアトラス全体の解像度 (512 - 8192)。2 の冪へ丸めはしない。
    FBZZ_ENGINE_API void SetShadowResolution(uint32_t resolution) const;
    [[nodiscard]] FBZZ_ENGINE_API uint32_t GetShadowResolution() const;
    /// @note カスケード分割数 (1 - 4)。
    FBZZ_ENGINE_API void SetShadowCascades(int count) const;
    [[nodiscard]] FBZZ_ENGINE_API int GetShadowCascades() const;

    FBZZ_ENGINE_API void SetSSR(bool enabled) const;
    [[nodiscard]] FBZZ_ENGINE_API bool GetSSR() const;
    FBZZ_ENGINE_API void SetGTAO(bool enabled) const;
    [[nodiscard]] FBZZ_ENGINE_API bool GetGTAO() const;
    FBZZ_ENGINE_API void SetSSAO(bool enabled) const;
    [[nodiscard]] FBZZ_ENGINE_API bool GetSSAO() const;
    FBZZ_ENGINE_API void SetTAA(bool enabled) const;
    [[nodiscard]] FBZZ_ENGINE_API bool GetTAA() const;
    FBZZ_ENGINE_API void SetFXAA(bool enabled) const;
    [[nodiscard]] FBZZ_ENGINE_API bool GetFXAA() const;
    FBZZ_ENGINE_API void SetBloom(bool enabled) const;
    [[nodiscard]] FBZZ_ENGINE_API bool GetBloom() const;
    FBZZ_ENGINE_API void SetMotionBlur(bool enabled) const;
    [[nodiscard]] FBZZ_ENGINE_API bool GetMotionBlur() const;
    FBZZ_ENGINE_API void SetVolumetricLight(bool enabled) const;
    [[nodiscard]] FBZZ_ENGINE_API bool GetVolumetricLight() const;
    FBZZ_ENGINE_API void SetContactShadow(bool enabled) const;
    [[nodiscard]] FBZZ_ENGINE_API bool GetContactShadow() const;
    /// @}
};

} /// @note namespace fbzz::scene
