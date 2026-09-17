/// @file    ScriptPostProcessProxy.hpp
/// @brief   Script からランタイム PostProcess 設定を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once
#include <Engine/Renderer/RenderSettings.hpp>
#include <cstdint>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptPostProcessProxy {
    Script* script = nullptr;

    /// @brief Script 専用のランタイム PostProcess 設定を取得する。ProjectSettings は書き換えず、
    ///        シーン再生中だけ有効な見た目調整として扱う。
    renderer::PostProcessSettings& Get() const;
    const renderer::PostProcessSettings* TryGet() const;
    void Set(const renderer::PostProcessSettings& settings) const;
    void Clear() const;

    /// @brief ユーザー定義 PostProcess を末尾に追加し、以後の編集用参照を返す。shaderPath は
    ///        フルスクリーン三角形の VSMain/PSMain を持つ HLSL を指定する。
    renderer::CustomPostProcessSettings& AddCustom(
        std::string_view name,
        std::string_view shaderPath = "assets/shaders/PostProcess/Custom/CustomPostProcess.hlsl",
        bool enabled = true) const;

    /// @brief 同名効果があれば再利用し、なければ AddCustom と同じ内容で作成する。
    /// @note OnStart/OnUpdate のどちらで呼んでも重複登録されない Script 向け API。
    renderer::CustomPostProcessSettings& EnsureCustom(
        std::string_view name,
        std::string_view shaderPath = "assets/shaders/PostProcess/Custom/CustomPostProcess.hlsl",
        bool enabled = true) const;

    /// FindCustom: 名前でユーザー定義 PostProcess を検索する。見つからない場合は nullptr。
    renderer::CustomPostProcessSettings* FindCustom(std::string_view name) const;

    /// RemoveCustom: 同名のユーザー定義 PostProcess を削除する。削除できた場合 true。
    bool RemoveCustom(std::string_view name) const;

    /// SetCustomEnabled: 同名効果の有効状態を切り替える。存在しない場合は false。
    bool SetCustomEnabled(std::string_view name, bool enabled) const;

    /// SetCustomParameter: パラメーター [index] を更新する。index は 0〜7。
    /// シェーダー側では 0〜3 が customParameters、4〜7 が customParameters2 に届く。
    bool SetCustomParameter(std::string_view name, uint32_t index, float value) const;

    /// SetCustomParameters: customParameters.xyzw (0〜3) をまとめて更新する。
    bool SetCustomParameters(std::string_view name, float x, float y, float z, float w) const;

    /// @brief PostProcessProfile (.fzdata) を読み込み、ランタイム設定を全置換する。シーン遷移や
    ///        演出変化に合わせてプロファイルを差し替えられるようにする。解決できない
    ///        (未存在 / 型不一致) 場合は false を返し、現在の設定を変更しない。
    /// @note 反映はプロファイルのポストプロセス部分のみ。SSR / GTAO / TAA / MotionBlur /
    ///       VolumetricLight / LensFlare / LUT は PostProcessVolumeComponent の profile 側。
    bool LoadProfile(std::string_view profilePath) const;
};

} // namespace fbzz::scene
