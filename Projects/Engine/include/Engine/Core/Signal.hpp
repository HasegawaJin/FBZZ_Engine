// FBZZ Engine
// Signal.hpp | fbzz
// 型安全な軽量シグナル (オブザーバー) ユーティリティ。
//
// 設計意図 (WHY):
//   スクリプト間連携は従来 scene.Find("Player_Sword_Trail")->GetScript<T>()->Method()
//   のように文字列名の合成で結合しており、リネームで壊れ・型安全でなかった。
//
//   Signal<Args...> は「発火側がイベントを type-safe に通知し、購読側がラムダで受ける」
//   観測者パターンを最小実装で提供する。グローバル状態を持たないため、EXE / Script DLL の
//   二重レジストリ問題 (ScriptFactory と同種の ABI 境界問題) を回避できる。
//   コンポーネント/スクリプトのメンバーとして公開イベントを表現するのに使う。
//
//   使用例 (発火側スクリプト):
//     fbzz::Signal<> onBloodSpray;                 // 引数なしイベントを公開
//     void OnHit() { onBloodSpray.Emit(); }
//
//   使用例 (購読側):
//     // FBZZ_REF(SwordTrailComponent, trail, "Trail") で参照を取得して購読
//     if (trail) trail->onBloodSpray.Connect([this]{ DoSomething(); });
#pragma once
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace fbzz {

// 購読解除に使う軽量ハンドル。
struct SignalConnection {
    uint32_t id = 0;
    bool IsValid() const { return id != 0; }
};

template<typename... Args>
class Signal {
public:
    using Slot = std::function<void(Args...)>;

    // 購読を追加する。返り値のハンドルで個別に解除できる。
    SignalConnection Connect(Slot slot)
    {
        const uint32_t id = m_nextId++;
        m_slots.push_back({ id, std::move(slot) });
        return { id };
    }

    // 個別解除。
    void Disconnect(SignalConnection c)
    {
        for (auto it = m_slots.begin(); it != m_slots.end(); ++it) {
            if (it->id == c.id) { m_slots.erase(it); return; }
        }
    }

    // 全解除。
    void Clear() { m_slots.clear(); }

    // 全購読者へ通知する。
    // WHY: 通知中に購読リストが変化しても安全なよう、スナップショットを走査する。
    void Emit(Args... args) const
    {
        const auto snapshot = m_slots;
        for (const auto& s : snapshot)
            if (s.fn) s.fn(args...);
    }

    bool Empty() const { return m_slots.empty(); }

private:
    struct Entry { uint32_t id; Slot fn; };
    std::vector<Entry> m_slots;
    uint32_t m_nextId = 1;
};

} // namespace fbzz
