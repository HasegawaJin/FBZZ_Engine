/// @file    ScriptEvent.hpp
/// @brief   スクリプト間の疎結合イベント (Pub/Sub)。
/// @author  Hasegawa Jin
/// @date    2026-08-16
///
/// @note GetScript<T>() で相手を直接掴むと参照方向が固定され相互依存になりやすい。1 対多の通知 (例: プレイヤー死亡→UI/SE/カメラ) を疎結合に書けるようにした。
/// @note チャンネル鍵に typeid でなく型が名乗る `EVENT_NAME` (文字列) を使う。スクリプトは DLL 側でコンパイルされ type_info が DLL とエンジン本体で別々に生成されるため。
#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace fbzz::scene {

class Script;

/// @brief イベント構造体にチャンネル名を与える。構造体の定義内へ 1 行置く。
/// @note 名前を手書きさせると打ち間違いが購読漏れとして静かに出るため、型名から自動生成し一意性を型システムに任せる。
#define FBZZ_EVENT(T) static constexpr const char* EVENT_NAME = #T

/// @brief Subscribe が返す購読ハンドル。個別解除が必要なときだけ保持すればよい。
/// @note 保持しなくても Script 破棄時に自動解除される。
struct ScriptEventToken {
    uint64_t id = 0;
    [[nodiscard]] bool IsValid() const { return id != 0; }
};

class ScriptEventBus {
public:
    using RawHandler = std::function<void(const void*)>;

    /// @brief Script に紐付けて購読する。owner が破棄されると自動的に解除される。
    /// @note owner が nullptr の場合は自動解除されないので、呼び出し側が Unsubscribe すること。
    static ScriptEventToken SubscribeRaw(Script* owner,
                                         std::string_view channel,
                                         RawHandler handler);
    static void Unsubscribe(ScriptEventToken token);
    /// @brief 指定 Script の購読をすべて解除する (~Script から呼ばれる)。
    static void UnsubscribeOwner(Script* owner);
    /// @brief 同フレーム内で同期的に全購読者へ配信する。
    static void PublishRaw(std::string_view channel, const void* payload);
    /// @brief 全購読を破棄する。スクリプト DLL のアンロード前に呼ぶ。
    static void Clear();

    /// @brief 現在の購読総数 (デバッグ用)。
    [[nodiscard]] static size_t SubscriptionCount();
};

} // namespace fbzz::scene
