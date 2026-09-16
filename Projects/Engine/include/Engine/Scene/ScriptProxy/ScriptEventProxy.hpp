/// @file    ScriptEventProxy.hpp
/// @brief   Script からスクリプト間イベントを発火・購読する。
/// @author  Hasegawa Jin
/// @date    2026-08-16
///
/// 設計意図は ScriptEvent.hpp を参照。ここは型安全な薄いラッパーで、
/// イベント構造体 ←→ void* の詰め替えだけを担う。
#pragma once

#include <Engine/Scene/ScriptEvent.hpp>
#include <concepts>
#include <functional>
#include <utility>

namespace fbzz::scene {

class Script;

// FBZZ_EVENT が付いているか (= EVENT_NAME を持つか) をコンパイル時に判定する。
// WHY concept にするか: 付け忘れたときのエラーを「テンプレート内部の深い場所」ではなく
//     呼び出し行で止め、何を直せばよいかを分かるようにする。
template<typename T>
concept ScriptEventType = requires { { T::EVENT_NAME } -> std::convertible_to<const char*>; };

struct ScriptEventProxy {
    Script* script = nullptr;

    // 購読する。この Script が破棄されると自動的に解除される。
    // 戻り値のトークンは「途中で降りたい」場合だけ保持すればよい。
    //
    //   events.Subscribe<PlayerDied>([this](const PlayerDied& e) { ShowResult(e.score); });
    template<ScriptEventType T, typename Fn>
        requires std::invocable<Fn, const T&>
    ScriptEventToken Subscribe(Fn&& fn) const
    {
        return ScriptEventBus::SubscribeRaw(
            script, T::EVENT_NAME,
            [handler = std::function<void(const T&)>(std::forward<Fn>(fn))](const void* payload) {
                handler(*static_cast<const T*>(payload));
            });
    }

    // 発火する。購読者はこの呼び出しの中で同期的に処理される。
    //
    //   events.Publish(PlayerDied{ .score = 1200 });
    template<ScriptEventType T>
    void Publish(const T& event) const
    {
        ScriptEventBus::PublishRaw(T::EVENT_NAME, &event);
    }

    // 個別解除。
    void Unsubscribe(ScriptEventToken token) const { ScriptEventBus::Unsubscribe(token); }
    // この Script の購読をすべて解除する (OnDisable で一旦降りる用途)。
    void UnsubscribeAll() const;
};

} // namespace fbzz::scene
