/// @file    ScriptIKProxy.hpp
/// @brief   Script から IKSolverComponent のチェーンを操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @note ターゲット名はチェーン識別ラベル。接続先の変更でも書き換えない。
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/Entity.hpp>
#include <string_view>

namespace fbzz::scene {

class Script;
class GameObject;

struct ScriptIKProxy {
    Script* script = nullptr;

    /// @brief ターゲット名でチェーンを探して有効/無効を切り替える。
    FBZZ_ENGINE_API void SetChainEnabled(std::string_view targetName, bool enabled) const;
    /// @return 名前が一致するチェーンが無ければ false。
    FBZZ_ENGINE_API bool IsChainEnabled(std::string_view targetName) const;
    FBZZ_ENGINE_API bool HasChain(std::string_view targetName) const;
    /// @brief 全チェーンの有効/無効をまとめて切り替える。
    FBZZ_ENGINE_API void SetAllEnabled(bool enabled) const;

    /// @brief ブレンドウェイトを設定する [0, 1]。0 で FK のみ、1 で完全 IK 適用。
    FBZZ_ENGINE_API void SetChainWeight(std::string_view targetName, float weight) const;
    FBZZ_ENGINE_API float GetChainWeight(std::string_view targetName) const;

    /// @brief IK ターゲット GameObject を動的に差し替える。
    FBZZ_ENGINE_API void SetChainTarget(std::string_view targetName, EntityID target) const;
    FBZZ_ENGINE_API void SetChainTarget(std::string_view targetName, const GameObject& target) const;
    /// @brief Poleを同じSceneのGOへ接続する。INVALIDは明示解除。
    FBZZ_ENGINE_API void SetChainPole(std::string_view targetName, EntityID target) const;
    FBZZ_ENGINE_API void SetChainPole(std::string_view targetName, const GameObject& target) const;

    /// @brief IKSolverComponent 自体の有効/無効。
    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;
    FBZZ_ENGINE_API bool IsEnabled() const;
};

} /// @note namespace fbzz::scene
