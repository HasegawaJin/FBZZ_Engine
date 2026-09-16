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

    // Get: Script 専用のランタイム PostProcess 設定を取得する。
    // WHY: ProjectSettings を直接書き換えず、シーン再生中だけ有効な見た目調整として扱う。
    renderer::PostProcessSettings& Get() const;
    const renderer::PostProcessSettings* TryGet() const;
    void Set(const renderer::PostProcessSettings& settings) const;
    void Clear() const;

    // AddCustom: ユーザー定義 PostProcess を末尾に追加し、以後の編集用参照を返す。
    // WHAT: shaderPath はフルスクリーン三角形の VSMain/PSMain を持つ HLSL を指定する。
    renderer::CustomPostProcessSettings& AddCustom(
        std::string_view name,
        std::string_view shaderPath = "assets/shaders/PostProcess/Custom/CustomPostProcess.hlsl",
        bool enabled = true) const;

    // EnsureCustom: 同名効果があれば再利用し、なければ AddCustom と同じ内容で作成する。
    // WHY: OnStart/OnUpdate のどちらで呼んでも重複登録されない Script 向け API にする。
    renderer::CustomPostProcessSettings& EnsureCustom(
        std::string_view name,
        std::string_view shaderPath = "assets/shaders/PostProcess/Custom/CustomPostProcess.hlsl",
        bool enabled = true) const;

    // FindCustom: 名前でユーザー定義 PostProcess を検索する。見つからない場合は nullptr。
    renderer::CustomPostProcessSettings* FindCustom(std::string_view name) const;

    // RemoveCustom: 同名のユーザー定義 PostProcess を削除する。削除できた場合 true。
    bool RemoveCustom(std::string_view name) const;

    // SetCustomEnabled: 同名効果の有効状態を切り替える。存在しない場合は false。
    bool SetCustomEnabled(std::string_view name, bool enabled) const;

    // SetCustomParameter: パラメーター [index] を更新する。index は 0〜7。
    // シェーダー側では 0〜3 が customParameters、4〜7 が customParameters2 に届く。
    bool SetCustomParameter(std::string_view name, uint32_t index, float value) const;

    // SetCustomParameters: customParameters.xyzw (0〜3) をまとめて更新する。
    bool SetCustomParameters(std::string_view name, float x, float y, float z, float w) const;

    // LoadProfile: PostProcessProfile (.fzdata) を読み込み、ランタイム設定を全置換する。
    // WHY: シーン遷移や演出変化に合わせてプロファイルをスクリプトから差し替えられるようにする。
    // 解決できない (未存在 / 型不一致) 場合は false を返し、現在の設定を変更しない。
    // NOTE: 反映されるのはプロファイルのポストプロセス部分のみ。ランタイム上書きの器が
    //       PostProcessSettings なので、SSR / GTAO / TAA / MotionBlur / VolumetricLight /
    //       LensFlare / LUT は含まれない。それらも効かせたい場合は
    //       PostProcessVolumeComponent の profile を差し替える。
    bool LoadProfile(std::string_view profilePath) const;
};

} // namespace fbzz::scene
