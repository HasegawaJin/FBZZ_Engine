/// @file    EventBus.hpp
/// @brief   型安全なグローバル Pub/Sub システム。
/// @author  Hasegawa Jin
/// @date    2026-05-24
///
/// type_index ごとに購読関数を管理し、イベント型を明示して Publish する。
/// SubscriberID は登録解除に必要なので呼び出し側で保持する。
#pragma once
#include <functional>
#include <typeindex>
#include <cstdint>

namespace fbzz::util {

class EventBus {
public:
    using SubscriberID = uint32_t;
    using RawHandler   = std::function<void(const void*)>;

    /// 購読登録。戻り値の ID を保持して Unsubscribe に渡す
    template<typename T>
    static SubscriberID Subscribe(std::function<void(const T&)> fn) {
        return SubscribeRaw(typeid(T), [fn](const void* ptr) {
            fn(*static_cast<const T*>(ptr));
        });
    }

    /// 購読解除
    template<typename T>
    static void Unsubscribe(SubscriberID id) {
        UnsubscribeRaw(typeid(T), id);
    }

    /// イベント発行。同フレーム内で同期呼び出しされる
    template<typename T>
    static void Publish(const T& event) {
        PublishRaw(typeid(T), &event);
    }

    /// Play モード開始/終了時など、全購読を一括解除する
    static void Clear();

private:
    static SubscriberID SubscribeRaw(std::type_index type, RawHandler handler);
    static void         UnsubscribeRaw(std::type_index type, SubscriberID id);
    static void         PublishRaw(std::type_index type, const void* event);
};

} // namespace fbzz::util
