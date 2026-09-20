/// @file    PipelineDiagnostics.hpp
/// @brief   現在のパイプラインでは効かない設定を列挙する
/// @author  Hasegawa Jin
/// @date    2026-08-25
/// @note SSAO を ON のまま Forward を選ぶといった「有効なのに黙って無視される設定」を検出し、
/// @note       効かない理由と直し方を明示する。判定述語 (RenderSettings::UsesGBuffer 等) はここに
/// @note       集約し、パス登録側と共有することで UI 側だけが古くなる食い違いを防ぐ。
#pragma once
#include <string_view>
#include <vector>

namespace fbzz::renderer {

struct RenderSettings;

/// @note 有効にしてあるのに現在のパイプラインでは無視される設定 1 件。
struct InertSetting {
    /// @note UI に出ている名前 (VolumeOverride の表示名や Project Settings の項目名)。
    const char* label = nullptr;
    /// @note なぜ効かないか。
    const char* reason = nullptr;
    /// @note どうすれば効くか。
    const char* remedy = nullptr;
};

/// @note 現在の設定から「有効なのに無視されるもの」を列挙する。
/// @note 何も無ければ空を返す。
[[nodiscard]] std::vector<InertSetting> CollectInertSettings(const RenderSettings& settings);

/// @note VolumeOverride 1 件が現在のパイプラインで効くかを名前で判定する。
/// @param typeName VolumeOverride::GetTypeName() の値 ("SSR" / "GTAO" など)
/// @return 効くなら nullptr。効かないなら理由の文字列 (表示用・静的寿命)。
[[nodiscard]] const char* DescribeInertOverride(
    const RenderSettings& settings, std::string_view typeName);

} /// @note namespace fbzz::renderer
