// FBZZ Engine
// ScriptIKProxy.hpp | fbzz::scene
// Script から IKSolverComponent のチェーンを操作するショートハンド。
// ターゲット名でチェーンを特定するため、Inspector で設定した targetName と一致させること。
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <string_view>

namespace fbzz::scene {

class Script;
class GameObject;

struct ScriptIKProxy {
    Script* script = nullptr;

    // ターゲット名でチェーンを探して有効/無効を切り替える。
    void SetChainEnabled(std::string_view targetName, bool enabled) const;
    // 全チェーンの有効/無効をまとめて切り替える。
    void SetAllEnabled(bool enabled) const;

    // ブレンドウェイトを設定する [0, 1]。0 で FK のみ、1 で完全 IK 適用。
    void SetChainWeight(std::string_view targetName, float weight) const;
    float GetChainWeight(std::string_view targetName) const;

    // IK ターゲット GameObject を動的に差し替える。
    void SetChainTarget(std::string_view targetName, EntityID target) const;
    void SetChainTarget(std::string_view targetName, const GameObject& target) const;

    // IKSolverComponent 自体の有効/無効。
    void SetEnabled(bool enabled) const;
};

} // namespace fbzz::scene
