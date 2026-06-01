// FBZZ Engine
// ScriptPostProcessProxy.hpp | fbzz::scene
// Script からランタイム PostProcess 設定を操作するショートハンド
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

    // SetCustomParameter: customParameters[index] を更新する。index は 0〜3。
    bool SetCustomParameter(std::string_view name, uint32_t index, float value) const;

    // SetCustomParameters: customParameters.xyzw をまとめて更新する。
    bool SetCustomParameters(std::string_view name, float x, float y, float z, float w) const;
};

} // namespace fbzz::scene
