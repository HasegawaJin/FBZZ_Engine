/// @file    QualityPreset.cpp
/// @brief   画質プリセットの内容と、現在設定からの逆引き。
/// @author  Hasegawa Jin
/// @date    2026-08-23
#include <Engine/Renderer/RenderSettings.hpp>

#include <cstddef>
#include <iterator>

namespace fbzz::renderer {

namespace {

/// プリセット 1 段ぶんの構成。RenderSettings のうち「重さ」に効く項目だけを持つ。
/// @note 4 段を縦に並べて読めるよう構造体に切り出している。関数内の switch で個別代入を
///       書くと段ごとの差分が目で追えなくなる。
struct PresetSpec {
    uint32_t shadowMapResolution;
    int      shadowCascades;
    int      shadowPcfRadius;
    bool     shadowPcss;
    bool     ssao;              ///< postProcess.ambientOcclusion (軽い方の AO)
    bool     gtao;              ///< 高品質 AO。ssao と排他
    bool     fxaa;              ///< 軽い方の AA
    bool     taa;               ///< 高品質 AA。fxaa と排他
    bool     ssr;
    bool     volumetricLight;
    bool     contactShadow;
    bool     motionBlur;
    bool     lensFlare;
    bool     bloom;
};

constexpr PresetSpec kPresets[] = {
    /// @note Low    影を 1 枚 512、AA は FXAA、AO 以外の追加パスはすべて切る。
    { 512u,  1, 0, false, false, false, true,  false, false, false, false, false, false, false },
    /// @note Medium 影 1024 / 2 カスケード、SSAO と Bloom を戻す。
    { 1024u, 2, 1, false, true,  false, true,  false, false, false, false, false, false, true  },
    /// @note High   既定値。影 2048 / 4 カスケード、GTAO + TAA、接触影まで。
    { 2048u, 4, 2, false, false, true,  false, true,  true,  false, true,  false, true,  true  },
    /// @note Ultra  影 4096 / PCSS、体積光とモーションブラーまで全部入り。
    { 4096u, 4, 3, true,  false, true,  false, true,  true,  true,  true,  true,  true,  true  },
};

[[nodiscard]] const PresetSpec& SpecOf(QualityPreset preset)
{
    const auto index = static_cast<size_t>(preset);
    return kPresets[index < std::size(kPresets) ? index : static_cast<size_t>(QualityPreset::High)];
}

[[nodiscard]] bool Matches(const RenderSettings& s, const PresetSpec& spec)
{
    return s.shadow.mapResolution              == spec.shadowMapResolution
        && s.shadow.cascadeCount               == spec.shadowCascades
        && s.shadow.pcfRadius                  == spec.shadowPcfRadius
        && s.shadow.pcssEnabled                == spec.shadowPcss
        && s.postProcess.ambientOcclusion.enabled == spec.ssao
        && s.gtao.enabled                      == spec.gtao
        && s.postProcess.fxaaEnabled           == spec.fxaa
        && s.taa.enabled                       == spec.taa
        && s.ssr.enabled                       == spec.ssr
        && s.volumetricLight.enabled           == spec.volumetricLight
        && s.contactShadow.enabled             == spec.contactShadow
        && s.motionBlur.enabled                == spec.motionBlur
        && s.lensFlare.enabled                 == spec.lensFlare
        && s.postProcess.bloom.enabled         == spec.bloom;
}

} // namespace

void ApplyQualityPreset(RenderSettings& settings, QualityPreset preset)
{
    const PresetSpec& spec = SpecOf(preset);

    settings.shadow.mapResolution = spec.shadowMapResolution;
    settings.shadow.cascadeCount  = spec.shadowCascades;
    settings.shadow.pcfRadius     = spec.shadowPcfRadius;
    settings.shadow.pcssEnabled   = spec.shadowPcss;

    settings.postProcess.ambientOcclusion.enabled = spec.ssao;
    settings.gtao.enabled                         = spec.gtao;
    settings.postProcess.fxaaEnabled              = spec.fxaa;
    settings.taa.enabled                          = spec.taa;

    settings.ssr.enabled             = spec.ssr;
    settings.volumetricLight.enabled = spec.volumetricLight;
    settings.contactShadow.enabled   = spec.contactShadow;
    settings.motionBlur.enabled      = spec.motionBlur;
    settings.lensFlare.enabled       = spec.lensFlare;
    settings.postProcess.bloom.enabled = spec.bloom;

    /// @note 排他スロット (FXAA/TAA, SSAO/GTAO) の規則は 1 か所に集約されている。
    ///       プリセットの表が正しくても、ここを通しておけば表の書き間違いで
    ///       両方立った状態が描画へ届くことはない。
    (void)settings.NormalizeExclusivePipelineSlots();
}

QualityPreset DetectQualityPreset(const RenderSettings& settings)
{
    for (size_t i = 0; i < std::size(kPresets); ++i) {
        if (Matches(settings, kPresets[i])) return static_cast<QualityPreset>(i);
    }
    /// @note どれとも一致しない = 個別に調整された状態。影の解像度を目安に
    ///       一番近い (超えない) 段を返す。Option 画面が「カスタム」を持たない前提の妥協。
    const uint32_t resolution = settings.shadow.mapResolution;
    if (resolution >= kPresets[static_cast<size_t>(QualityPreset::Ultra)].shadowMapResolution)
        return QualityPreset::Ultra;
    if (resolution >= kPresets[static_cast<size_t>(QualityPreset::High)].shadowMapResolution)
        return QualityPreset::High;
    if (resolution >= kPresets[static_cast<size_t>(QualityPreset::Medium)].shadowMapResolution)
        return QualityPreset::Medium;
    return QualityPreset::Low;
}

} // namespace fbzz::renderer
