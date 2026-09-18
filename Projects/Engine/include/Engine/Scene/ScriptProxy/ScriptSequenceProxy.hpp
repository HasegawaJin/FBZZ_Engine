/// @file    ScriptSequenceProxy.hpp
/// @brief   Script から .sequence の再生とバインディングを操作する
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once

#include <string_view>

namespace fbzz::scene {

class Script;
class GameObject;

/// 既定の対象は同じ GameObject の SequencePlayerComponent。
/// 別オブジェクトを回したいときだけ *On 系を使う。
struct ScriptSequenceProxy {
    explicit ScriptSequenceProxy(Script* owner) : script(owner) {}
    Script* script = nullptr;

    /// path を差し替えて先頭から再生する。SequencePlayerComponent が無ければ追加する。
    void Play(std::string_view path) const;
    void Play() const;
    void Stop() const;
    void Pause() const;
    void Resume() const;
    void SetTime(float seconds) const;
    void SetSpeed(float speed) const;

    /// binding キーへ実体を割り当てる。target が null ならキーを未解決へ戻す。
    /// @note タグ解決をエンジンでやらない理由: "Player" タグを引くのはゲームの知識で、
    ///       エンジンが持つと .sequence がゲームの命名規約に依存してしまうため。
    void Bind(std::string_view key, GameObject* target) const;
    void ClearBindings() const;

    [[nodiscard]] bool  IsPlaying() const;
    [[nodiscard]] float Time() const;
    [[nodiscard]] float Duration() const;

    void PlayOn(GameObject& owner, std::string_view path) const;
    void StopOn(GameObject& owner) const;
    void BindOn(GameObject& owner, std::string_view key, GameObject* target) const;
};

} // namespace fbzz::scene
