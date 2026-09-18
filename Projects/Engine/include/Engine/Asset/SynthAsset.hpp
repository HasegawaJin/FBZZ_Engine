/// @file    SynthAsset.hpp
/// @brief   .synth 手続き効果音アセットのランタイム表現と TOML 入出力。
/// @author  Hasegawa Jin
/// @date    2026-08-23
#pragma once
#include <Engine/Audio/SynthSpec.hpp>
/// @note `IReflector` (Inspector / TOML 共用の反射)。
#include <Engine/Scene/Script.hpp>
#include <string>
#include <string_view>

namespace fbzz::asset {

/// .synth の内容。AudioManager が読み込み時に PCM へ合成するため、
/// 参照側からは .wav などの録音素材と区別せずに clipPath で扱える。
struct SynthAsset {
    audio::SynthSpec spec;

    /// 由来プリセット名 (空 = 手動編集)。合成結果には影響せず、
    /// 「どこから作ったか」を Editor で示すためだけに保持する。
    std::string presetName;

    const char* GetTypeName() const { return "Synth Clip"; }

    void Reflect(scene::IReflector& r);
};

/// TOML から .synth を読む。未存在・破損時は false を返し、out は変更しない。
[[nodiscard]] bool LoadSynthAssetFromFile(std::string_view path, SynthAsset& outAsset);

/// .synth を TOML へ保存する。既定値と同じ項目は書き出さない。
[[nodiscard]] bool SaveSynthAssetToFile(std::string_view path, const SynthAsset& asset);

} // namespace fbzz::asset
