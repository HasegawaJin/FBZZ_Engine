/// @file    EventBus.hpp
/// @brief   型安全なグローバル Pub/Sub システム。
/// @author  Hasegawa Jin
/// @date    2026-05-24

/// @note type_index ごとに購読関数を管理し、イベント型を明示して Publish する。
/// @note SubscriberID は登録解除に必要なので呼び出し側で保持する。
#pragma once
#include <Engine/Api.hpp>
#include <functional>
#include <typeindex>
#include <cstdint>

namespace fbzz::util {

class EventBus {
public:
    using SubscriberID = uint32_t;
    using RawHandler   = std::function<void(const void*)>;

    /// @note 購読登録。戻り値の ID を保持して Unsubscribe に渡す
    template<typename T>
    static SubscriberID Subscribe(std::function<void(const T&)> fn) {
        return SubscribeRaw(typeid(T), [fn](const void* ptr) {
            fn(*static_cast<const T*>(ptr));
        });
    }

    /// @note 購読解除
    template<typename T>
    static void Unsubscribe(SubscriberID id) {
        UnsubscribeRaw(typeid(T), id);
    }

    /// @note イベント発行。同フレーム内で同期呼び出しされる
    template<typename T>
    static void Publish(const T& event) {
        PublishRaw(typeid(T), &event);
    }

    /// @note Play モード開始/終了時など、全購読を一括解除する
    FBZZ_ENGINE_API static void Clear();

private:
    FBZZ_ENGINE_API static SubscriberID SubscribeRaw(std::type_index type, RawHandler handler);
    FBZZ_ENGINE_API static void         UnsubscribeRaw(std::type_index type, SubscriberID id);
    FBZZ_ENGINE_API static void         PublishRaw(std::type_index type, const void* event);
};

} /// @note namespace fbzz::util
