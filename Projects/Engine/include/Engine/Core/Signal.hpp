/// @file    Signal.hpp
/// @brief   型安全な軽量シグナル (オブザーバー) ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-06-29
///
/// @note スクリプト間連携が scene.Find(...)->GetScript<T>()->Method() のような文字列合成に頼るとリネームで壊れ型安全でない。Signal<Args...> は発火側が type-safe に通知し購読側がラムダで受ける観測者パターンを提供する。
/// @note グローバル状態を持たないため EXE/Script DLL の二重レジストリ問題 (ScriptFactory と同種の ABI 境界問題) を回避できる。コンポーネント/スクリプトのメンバーとして公開イベントを表現するのに使う。
#pragma once
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace fbzz {

/// @brief 購読解除に使う軽量ハンドル。
struct SignalConnection {
    uint32_t id = 0;
    bool IsValid() const { return id != 0; }
};

template<typename... Args>
class Signal {
public:
    using Slot = std::function<void(Args...)>;

    /// @brief 購読を追加する。
    /// @return 返り値のハンドルで個別に解除できる。
    SignalConnection Connect(Slot slot)
    {
        const uint32_t id = m_nextId++;
        m_slots.push_back({ id, std::move(slot) });
        return { id };
    }

    /// @brief 個別解除。
    void Disconnect(SignalConnection c)
    {
        for (auto it = m_slots.begin(); it != m_slots.end(); ++it) {
            if (it->id == c.id) { m_slots.erase(it); return; }
        }
    }

    /// @brief 全解除。
    void Clear() { m_slots.clear(); }

    /// @brief 全購読者へ通知する。
    /// @note 通知中に購読リストが変化しても安全なよう、スナップショットを走査する。
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
