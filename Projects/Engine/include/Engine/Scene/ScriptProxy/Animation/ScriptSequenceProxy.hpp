/// @file    ScriptSequenceProxy.hpp
/// @brief   Script から .sequence の再生とバインディングを操作する
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once
#include <Engine/Api.hpp>

#include <string_view>

namespace fbzz::scene {

class Script;
class GameObject;

/// @note 既定の対象は同じ GameObject の SequencePlayerComponent。
/// @note 別オブジェクトを回したいときだけ *On 系を使う。
struct ScriptSequenceProxy {
    explicit ScriptSequenceProxy(Script* owner) : script(owner) {}
    Script* script = nullptr;

    /// @note path を差し替えて先頭から再生する。SequencePlayerComponent が無ければ追加する。
    FBZZ_ENGINE_API void Play(std::string_view path) const;
    FBZZ_ENGINE_API void Play() const;
    FBZZ_ENGINE_API void Stop() const;
    FBZZ_ENGINE_API void Pause() const;
    FBZZ_ENGINE_API void Resume() const;
    FBZZ_ENGINE_API void SetTime(float seconds) const;
    FBZZ_ENGINE_API void SetSpeed(float speed) const;

    /// @note binding キーへ実体を割り当てる。target が null ならキーを未解決へ戻す。
    /// @note タグ解決をエンジンでやらない理由: "Player" タグを引くのはゲームの知識で、
    /// @note エンジンが持つと .sequence がゲームの命名規約に依存してしまうため。
    FBZZ_ENGINE_API void Bind(std::string_view key, GameObject* target) const;
    FBZZ_ENGINE_API void ClearBindings() const;

    [[nodiscard]] FBZZ_ENGINE_API bool  IsPlaying() const;
    [[nodiscard]] FBZZ_ENGINE_API float Time() const;
    [[nodiscard]] FBZZ_ENGINE_API float Duration() const;

    FBZZ_ENGINE_API void PlayOn(GameObject& owner, std::string_view path) const;
    FBZZ_ENGINE_API void StopOn(GameObject& owner) const;
    FBZZ_ENGINE_API void BindOn(GameObject& owner, std::string_view key, GameObject* target) const;
};

} /// @note namespace fbzz::scene
