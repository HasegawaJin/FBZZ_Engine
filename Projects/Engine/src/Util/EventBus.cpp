/// @file    EventBus.cpp
/// @brief   EventBus の購読管理実装。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include <Engine/Util/EventBus.hpp>
#include <memory>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace fbzz::util {

namespace {

struct HandlerEntry {
    EventBus::SubscriberID id;
    /// @note 購読表と開始済み配信が同一の callable を所有し、mutable な状態と解除中の寿命を保つ。
    std::shared_ptr<EventBus::RawHandler> fn;
};

std::unordered_map<std::type_index, std::vector<HandlerEntry>> s_handlers;
EventBus::SubscriberID s_nextId = 1;

} /// @note namespace

EventBus::SubscriberID EventBus::SubscribeRaw(std::type_index type, RawHandler handler)
{
    if (s_nextId == 0) return 0;
    SubscriberID id = s_nextId++;
    s_handlers[type].push_back({ id, std::make_shared<RawHandler>(std::move(handler)) });
    return id;
}

void EventBus::UnsubscribeRaw(std::type_index type, SubscriberID id)
{
    auto it = s_handlers.find(type);
    if (it == s_handlers.end()) return;
    auto& vec = it->second;
    vec.erase(std::remove_if(vec.begin(), vec.end(),
        [id](const HandlerEntry& e) { return e.id == id; }), vec.end());
}

void EventBus::PublishRaw(std::type_index type, const void* event)
{
    auto it = s_handlers.find(type);
    if (it == s_handlers.end()) return;
    /// @note 開始時の購読列を保持し、callback 本体を複製せずに解除中の走査を保護する。
    auto handlers = it->second;
    for (const auto& e : handlers)
        (*e.fn)(event);
}

void EventBus::Clear()
{
    s_handlers.clear();
}

} /// @note namespace fbzz::util
