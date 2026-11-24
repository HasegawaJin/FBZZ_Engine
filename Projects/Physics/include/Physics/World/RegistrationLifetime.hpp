/// @file    RegistrationLifetime.hpp
/// @brief   外部所有の物理個体を World が安全に借用するための寿命証明。
/// @author  Hasegawa Jin
/// @date    2026-11-01
#pragma once
#include <memory>

namespace fbzz::physics::detail {

/// @note 個体の所有権を共有せず、登録中だけ独立した証明を作る。コピー・移動は登録を引き継がない。
/// @pre 生成・変更・破棄と World の同期は所有スレッドで行い、並行 read query と重ねない。
class RegistrationLifetime {
public:
    RegistrationLifetime() = default;
    RegistrationLifetime(const RegistrationLifetime&) noexcept {}
    RegistrationLifetime(RegistrationLifetime&& other) noexcept { other.m_token.reset(); }
    RegistrationLifetime& operator=(const RegistrationLifetime& other) noexcept
    {
        if (this != &other) m_token.reset();
        return *this;
    }
    RegistrationLifetime& operator=(RegistrationLifetime&& other) noexcept
    {
        if (this != &other) { m_token.reset(); other.m_token.reset(); }
        return *this;
    }

    [[nodiscard]] std::weak_ptr<const void> Observe() const
    {
        if (!m_token) m_token = std::make_shared<const unsigned char>(0);
        return m_token;
    }

private:
    mutable std::shared_ptr<const void> m_token;
};

} /// @note namespace fbzz::physics::detail
