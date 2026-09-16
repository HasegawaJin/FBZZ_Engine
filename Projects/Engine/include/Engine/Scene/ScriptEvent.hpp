/// @file    ScriptEvent.hpp
/// @brief   スクリプト間の疎結合イベント (Pub/Sub)。
/// @author  Hasegawa Jin
/// @date    2026-08-16
///
/// 設計意図 (WHY):
/// これまでスクリプト同士の連絡は GetScript<T>() で相手を直接掴んで呼ぶしかなく、
/// 「プレイヤー死亡 → UI・SE・カメラがそれぞれ反応する」のような 1 対多の通知が書けなかった。
/// 発火側が受け手の型を知る必要がある = 参照方向が固定され、UI がプレイヤーを知り、
/// プレイヤーが UI を知る、という相互依存になりやすい。
///
/// util::EventBus との違い (WHY 別実装か):
/// 1. チャンネル鍵に typeid ではなく型が自分で名乗る EVENT_NAME を使う。
/// スクリプトは DLL 側でコンパイルされるため、DLL とエンジン本体で別々に
/// 生成された type_info を突き合わせることになる。文字列名なら確実に一致する。
/// 2. 購読が Script の寿命に紐付く。Script が破棄されると ~Script →
/// CancelEventSubscriptions() が購読を自動解除するので、DLL ホットリロードで
/// 解放済みコードを指すハンドラが残らない。util::EventBus は ID を呼び出し側が
/// 保持して自分で解除する設計なので、この保証が無い。
///
/// 使い方:
/// struct PlayerDied { FBZZ_EVENT(PlayerDied); int score = 0; };
///
/// // 受け手 (購読は Script が死ぬと自動解除される)
/// void OnStart() override {
/// events.Subscribe<PlayerDied>([this](const PlayerDied& e) { ShowResult(e.score); });
/// }
/// // 発火側
/// events.Publish(PlayerDied{ .score = 1200 });
#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace fbzz::scene {

class Script;

// イベント構造体にチャンネル名を与える。構造体の定義内へ 1 行置く。
// WHY 型名をそのまま使うか: 名前を手で書かせると打ち間違いが購読漏れとして
//     静かに出るため、型名から自動生成して一意性を型システムに任せる。
#define FBZZ_EVENT(T) static constexpr const char* EVENT_NAME = #T

// Subscribe が返す購読ハンドル。個別解除が必要なときだけ保持すればよい
// (保持しなくても Script 破棄時に自動解除される)。
struct ScriptEventToken {
    uint64_t id = 0;
    [[nodiscard]] bool IsValid() const { return id != 0; }
};

class ScriptEventBus {
public:
    using RawHandler = std::function<void(const void*)>;

    // Script に紐付けて購読する。owner が破棄されると自動的に解除される。
    // owner が nullptr の場合は自動解除されないので、呼び出し側が Unsubscribe すること。
    static ScriptEventToken SubscribeRaw(Script* owner,
                                         std::string_view channel,
                                         RawHandler handler);
    static void Unsubscribe(ScriptEventToken token);
    // 指定 Script の購読をすべて解除する (~Script から呼ばれる)。
    static void UnsubscribeOwner(Script* owner);
    // 同フレーム内で同期的に全購読者へ配信する。
    static void PublishRaw(std::string_view channel, const void* payload);
    // 全購読を破棄する。スクリプト DLL のアンロード前に呼ぶ。
    static void Clear();

    // デバッグ用: 現在の購読総数。
    [[nodiscard]] static size_t SubscriptionCount();
};

} // namespace fbzz::scene
