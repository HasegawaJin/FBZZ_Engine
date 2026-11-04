/// @file    ScriptPostProcessProxy.hpp
/// @brief   Script からランタイム PostProcess 設定を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <cstdint>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptPostProcessProxy {
    Script* script = nullptr;

    /// @brief Script 専用のランタイム PostProcess 設定を取得する。ProjectSettings は書き換えず、
    /// @note シーン再生中だけ有効な見た目調整として扱う。
    FBZZ_ENGINE_API renderer::PostProcessSettings& Get() const;
    FBZZ_ENGINE_API const renderer::PostProcessSettings* TryGet() const;
    FBZZ_ENGINE_API void Set(const renderer::PostProcessSettings& settings) const;
    FBZZ_ENGINE_API void Clear() const;

    /// @brief ユーザー定義 PostProcess を末尾に追加し、以後の編集用参照を返す。shaderPath は
    /// @note フルスクリーン三角形の VSMain/PSMain を持つ HLSL を指定する。
    FBZZ_ENGINE_API renderer::CustomPostProcessSettings& AddCustom(
        std::string_view name,
        std::string_view shaderPath = "assets/shaders/PostProcess/Custom/CustomPostProcess.hlsl",
        bool enabled = true) const;

    /// @brief 同名効果があれば再利用し、なければ AddCustom と同じ内容で作成する。
    /// @note OnStart/OnUpdate のどちらで呼んでも重複登録されない Script 向け API。
    FBZZ_ENGINE_API renderer::CustomPostProcessSettings& EnsureCustom(
        std::string_view name,
        std::string_view shaderPath = "assets/shaders/PostProcess/Custom/CustomPostProcess.hlsl",
        bool enabled = true) const;

    /// @note FindCustom: 名前でユーザー定義 PostProcess を検索する。見つからない場合は nullptr。
    FBZZ_ENGINE_API renderer::CustomPostProcessSettings* FindCustom(std::string_view name) const;

    /// @note RemoveCustom: 同名のユーザー定義 PostProcess を削除する。削除できた場合 true。
    FBZZ_ENGINE_API bool RemoveCustom(std::string_view name) const;

    /// @note SetCustomEnabled: 同名効果の有効状態を切り替える。存在しない場合は false。
    FBZZ_ENGINE_API bool SetCustomEnabled(std::string_view name, bool enabled) const;

    /// @note SetCustomParameter: パラメーター [index] を更新する。index は 0〜7。
    /// @note シェーダー側では 0〜3 が customParameters、4〜7 が customParameters2 に届く。
    FBZZ_ENGINE_API bool SetCustomParameter(std::string_view name, uint32_t index, float value) const;

    /// @note SetCustomParameters: customParameters.xyzw (0〜3) をまとめて更新する。
    FBZZ_ENGINE_API bool SetCustomParameters(std::string_view name, float x, float y, float z, float w) const;

    /// @brief PostProcessProfile (.fzdata) を読み込み、ランタイム設定を全置換する。シーン遷移や
    /// @note 演出変化に合わせてプロファイルを差し替えられるようにする。解決できない
    /// @note (未存在 / 型不一致) 場合は false を返し、現在の設定を変更しない。
    /// @note 反映はプロファイルのポストプロセス部分のみ。SSR / GTAO / TAA / MotionBlur /
    /// @note VolumetricLight / LensFlare / LUT は PostProcessVolumeComponent の profile 側。
    FBZZ_ENGINE_API bool LoadProfile(std::string_view profilePath) const;
};

} /// @note namespace fbzz::scene
