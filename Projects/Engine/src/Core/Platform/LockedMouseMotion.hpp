/// @file    LockedMouseMotion.hpp
/// @brief   カーソル拘束中に届いた相対移動を入力フレーム単位で集約する。
/// @author  Hasegawa Jin
/// @date    2026-11-09
#pragma once
#include <Math/Vector2.hpp>
#include <optional>
#include <utility>

namespace fbzz::core::cursor_detail {

class LockedMouseMotion final {
public:
    /// @note ボタンやホイールだけのパケットは座標差へのフォールバックを妨げない。
    void Add(int x, int y)
    {
        if (x == 0 && y == 0) return;
        if (!m_delta) m_delta.emplace();
        *m_delta += math::Vector2{static_cast<float>(x), static_cast<float>(y)};
    }

    /// @return 相対移動のパケットが無かったフレームだけ nullopt。
    [[nodiscard]] std::optional<math::Vector2> Take()
    {
        return std::exchange(m_delta, std::nullopt);
    }

private:
    std::optional<math::Vector2> m_delta;
};

/// @pre メインスレッドで Win32 の相対移動パケットを受信したときに呼ぶ。
void HandleRelativeMouseMotion(int x, int y);

} /// @note namespace fbzz::core::cursor_detail
