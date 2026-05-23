// FBZZ Engine
// EventBus.cpp | fbzz::util
#include <Engine/Util/EventBus.hpp>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace fbzz::util {

namespace {

struct HandlerEntry {
    EventBus::SubscriberID id;
    EventBus::RawHandler   fn;
};

std::unordered_map<std::type_index, std::vector<HandlerEntry>> s_handlers;
EventBus::SubscriberID s_nextId = 1;

} // namespace

EventBus::SubscriberID EventBus::SubscribeRaw(std::type_index type, RawHandler handler)
{
    SubscriberID id = s_nextId++;
    s_handlers[type].push_back({ id, std::move(handler) });
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
    // コピーしてからイテレート — ハンドラ内で Unsubscribe しても安全
    auto handlers = it->second;
    for (const auto& e : handlers)
        e.fn(event);
}

void EventBus::Clear()
{
    s_handlers.clear();
    s_nextId = 1;
}

} // namespace fbzz::util
