/// @file    ScriptIKProxy.hpp
/// @brief   Script から IKSolverComponent のチェーンを操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// ターゲット名でチェーンを特定するため、Inspector で設定した targetName と一致させること。
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <string_view>

namespace fbzz::scene {

class Script;
class GameObject;

struct ScriptIKProxy {
    Script* script = nullptr;

    /// @brief ターゲット名でチェーンを探して有効/無効を切り替える。
    void SetChainEnabled(std::string_view targetName, bool enabled) const;
    /// @return 名前が一致するチェーンが無ければ false。
    bool IsChainEnabled(std::string_view targetName) const;
    bool HasChain(std::string_view targetName) const;
    /// @brief 全チェーンの有効/無効をまとめて切り替える。
    void SetAllEnabled(bool enabled) const;

    /// @brief ブレンドウェイトを設定する [0, 1]。0 で FK のみ、1 で完全 IK 適用。
    void SetChainWeight(std::string_view targetName, float weight) const;
    float GetChainWeight(std::string_view targetName) const;

    /// @brief IK ターゲット GameObject を動的に差し替える。
    void SetChainTarget(std::string_view targetName, EntityID target) const;
    void SetChainTarget(std::string_view targetName, const GameObject& target) const;

    /// @brief IKSolverComponent 自体の有効/無効。
    void SetEnabled(bool enabled) const;
    bool IsEnabled() const;
};

} // namespace fbzz::scene
