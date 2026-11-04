/// @file    ScriptEvent.cpp
/// @brief   スクリプト間イベントバスの実装。
/// @author  Hasegawa Jin
/// @date    2026-08-16
#include <Engine/Scene/ScriptEvent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Profiler/ScriptProfiler.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {
namespace {

struct Subscription {
    uint64_t                    id      = 0;
    Script*                     owner   = nullptr;
    /// @note 購読と進行中の配信が同じ callback を所有し、再確保や解除で状態を複製・破棄しない。
    std::shared_ptr<ScriptEventBus::RawHandler> handler;
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
/// @note 配信中は購読の物理削除を遅延し、ネストした Publish の深さを保持する。
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
    if (channel.empty() || !handler || g_nextId == 0) return {};

    Subscription subscription{};
    subscription.id      = g_nextId++;
    subscription.owner   = owner;
    subscription.handler = std::make_shared<RawHandler>(std::move(handler));

    const ScriptEventToken token{ subscription.id };
    Channels()[std::string(channel)].push_back(std::move(subscription));

    /// @note Script の破棄は UnsubscribeOwner(this) で一括解除し、購読ごとの解除関数を重複登録しない。
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
    /// @note 開始時点の件数までを走査し、配信中に増えた購読は次回の Publish から届ける。
    const size_t initialCount = it->second.size();
    for (size_t i = 0; i < initialCount; ++i) {
        /// @note 呼出し側の文字列は callback が変更し得るため、登録表が所有する鍵で取り直す。
        auto& subs = Channels()[channelName];
        if (i >= subs.size()) break;
        if (subs[i].canceled) continue;
        /// @note 持ち主のスクリプトが無効 (自身が無効・親ごと無効化) なら配らない。購読は残すので、有効に戻せば何もせずまた届く。
        /// @note Update と同じ規則にしないと、止めたはずのオブジェクトがイベントにだけ反応し続ける。
        if (const Script* owner = subs[i].owner; owner && !owner->scene.IsActiveAndEnabled()) continue;
        const auto handler = subs[i].handler;
        if (handler && *handler) {
            if (auto* owner = subs[i].owner) {
                owner->ExecuteProfiledCallback([&] { (*handler)(payload); }, ScriptCallbackKind::EVENT_HANDLER, channelName.c_str());
            } else (*handler)(payload);
        }
    }
    --g_publishDepth;

    CompactIfIdle();
}

void ScriptEventBus::Clear()
{
    /// @note Clear も個別解除と同じ安全点で破棄し、進行中の鍵参照・件数・再入深さを保持する。
    for (auto& [channel, subs] : Channels())
        for (auto& subscription : subs)
            subscription.canceled = true;
    CompactIfIdle();
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
