/// @file    ScopedComApartment.hpp
/// @brief   WIC を呼ぶスレッドの COM 初期化を管理する。
/// @author  Hasegawa Jin
/// @date    2026-11-17
#pragma once

namespace fbzz::renderer::detail {
class ScopedComApartment final {
public:
    ScopedComApartment();
    ~ScopedComApartment();
    ScopedComApartment(const ScopedComApartment&) = delete;
    ScopedComApartment& operator=(const ScopedComApartment&) = delete;
    [[nodiscard]] bool IsUsable() const { return m_usable; }

private:
    bool m_ownsApartment = false;
    bool m_usable = false;
};
} /// @note namespace fbzz::renderer::detail
