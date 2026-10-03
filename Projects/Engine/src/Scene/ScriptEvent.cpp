/// @file    ScriptEvent.cpp
/// @brief   スクリプト間イベントバスの実装。
/// @author  Hasegawa Jin
/// @date    2026-08-16
#include <Engine/Scene/ScriptEvent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Profiler/ScriptProfiler.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {
namespace {

struct Subscription {
    uint64_t                    id      = 0;
    Script*                     owner   = nullptr;
    ScriptEventBus::RawHandler  handler;
    bool                        canceled = false;
};

/// @note チャンネル名 → 購読リスト。
/// @note 配信はフレームごとに走る一方、購読/解除は開始時と破棄時に偏るため、連続領域を順に舐める
/// @note 配信コストを優先し、解除は canceled フラグ + 後片付けで行う。
std::unordered_map<std::string, std::vector<Subscription>>& Channels()
{
    static std::unordered_map<std::string, std::vector<Subscription>> s_channels;
    return s_channels;
}

uint64_t g_nextId = 1;
/// @note 配信中は vector を再確保させない。ネストした Publish もあり得るので深さで数える。
int g_publishDepth = 0;

/// @note canceled になった購読を実際に取り除く。配信の入れ子が完全に抜けたときだけ行う。
void CompactIfIdle()
{
    if (g_publishDepth > 0) return;
    for (auto it = Channels().begin(); it != Channels().end();) {
        auto& subs = it->second;
        subs.erase(std::remove_if(subs.begin(), subs.end(),
                                  [](const Subscription& s) { return s.canceled; }),
                   subs.end());
        it = subs.empty() ? Channels().erase(it) : std::next(it);
    }
}

} /// @note namespace

ScriptEventToken ScriptEventBus::SubscribeRaw(Script* owner,
                                              std::string_view channel,
                                              RawHandler handler)
{
    if (channel.empty() || !handler) return {};

    Subscription subscription{};
    subscription.id      = g_nextId++;
    subscription.owner   = owner;
    subscription.handler = std::move(handler);

    const ScriptEventToken token{ subscription.id };
    Channels()[std::string(channel)].push_back(std::move(subscription));

    /// @note 自動解除は Script::CancelEventSubscriptions() が UnsubscribeOwner(this) を呼ぶことで
    /// @note 行われる (~Script から必ず通る)。Subscribe ごとに解除関数を積むと購読数ぶんの重複
    /// @note エントリになるため、オーナー単位の一括解除にまとめる。
    return token;
}

void ScriptEventBus::Unsubscribe(ScriptEventToken token)
{
    if (!token.IsValid()) return;
    for (auto& [channel, subs] : Channels()) {
        for (auto& subscription : subs) {
            if (subscription.id != token.id) continue;
            subscription.canceled = true;
            CompactIfIdle();
            return;
        }
    }
}

void ScriptEventBus::UnsubscribeOwner(Script* owner)
{
    if (!owner) return;
    for (auto& [channel, subs] : Channels())
        for (auto& subscription : subs)
            if (subscription.owner == owner)
                subscription.canceled = true;
    CompactIfIdle();
}

void ScriptEventBus::PublishRaw(std::string_view channel, const void* payload)
{
    const auto it = Channels().find(std::string(channel));
    if (it == Channels().end()) return;
    /// @note callback 内の別 channel 登録による rehash 後も、node の文字列参照は有効。
    /// @see https://eel.is/c++draft/unord.req.general#9 rehash による iterator 失効と要素参照の契約。
    const std::string& channelName = it->first;

    ++g_publishDepth;
    /// @note ハンドラ内から Subscribe されると vector が再確保され得るため、開始時点の件数までを
    /// @note 走査する。配信中に増えた購読は次回の Publish から届かせ、同一イベント配信中に自分自身を
    /// @note 購読して即受け取る混乱を避ける。
    const size_t initialCount = it->second.size();
    for (size_t i = 0; i < initialCount; ++i) {
        /// @note 参照は保持しない。ハンドラ内の Subscribe による再確保後も安全に読めるよう、
        /// @note 毎回コンテナ経由で取り直してからコピーして呼ぶ。
        auto& subs = Channels()[std::string(channel)];
        if (i >= subs.size()) break;
        if (subs[i].canceled) continue;
        /// @note 持ち主のスクリプトが無効 (自身が無効・親ごと無効化) なら配らない。購読は残すので、有効に戻せば何もせずまた届く。
        /// @note Update と同じ規則にしないと、止めたはずのオブジェクトがイベントにだけ反応し続ける。
        if (const Script* owner = subs[i].owner; owner && !owner->scene.IsActiveAndEnabled()) continue;
        RawHandler handler = subs[i].handler;
        if (handler) {
            if (auto* owner = subs[i].owner) {
                owner->ExecuteProfiledCallback([&] { handler(payload); }, ScriptCallbackKind::EVENT_HANDLER, channelName.c_str());
            } else handler(payload);
        }
    }
    --g_publishDepth;

    CompactIfIdle();
}

void ScriptEventBus::Clear()
{
    Channels().clear();
    g_nextId = 1;
    g_publishDepth = 0;
}

size_t ScriptEventBus::SubscriptionCount()
{
    size_t count = 0;
    for (const auto& [channel, subs] : Channels())
        count += std::count_if(subs.begin(), subs.end(),
                               [](const Subscription& s) { return !s.canceled; });
    return count;
}

} /// @note namespace fbzz::scene
