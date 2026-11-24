/// @file    RenderObjectSelection.hpp
/// @brief   ビュー固有の選択フラグと、公開時に確定したメッシュの添字。
/// @author  Hasegawa Jin
/// @date    2026-11-23
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace fbzz::renderer {

/// @note 編集可能なコピーや移動先では添字を再利用しない。選択ゼロを確定済みの公開値は空の span を返す。
class RenderObjectSelection {
public:
    RenderObjectSelection() = default;
    RenderObjectSelection(const RenderObjectSelection& other) : m_flags(other.m_flags) {}
    RenderObjectSelection(RenderObjectSelection&& other) noexcept : m_flags(std::move(other.EditFlags())) {}
    RenderObjectSelection& operator=(const RenderObjectSelection& other)
    {
        if (this != &other) EditFlags() = other.m_flags;
        return *this;
    }
    RenderObjectSelection& operator=(RenderObjectSelection&& other) noexcept
    {
        if (this != &other) EditFlags() = std::move(other.EditFlags());
        return *this;
    }

    /// @note 空なら各 RenderObject の selected、それ以外は Objects() 順のフラグを使う。
    [[nodiscard]] std::span<const uint8_t> Flags() const { return m_flags; }
    std::vector<uint8_t>& EditFlags() noexcept
    {
        m_ordinals.reset();
        return m_flags;
    }
    void Publish(std::vector<uint32_t> ordinals) { m_ordinals = std::move(ordinals); }
    [[nodiscard]] std::optional<std::span<const uint32_t>> Published() const
    {
        if (!m_ordinals) return std::nullopt;
        return std::span<const uint32_t>(*m_ordinals);
    }
private:
    std::vector<uint8_t> m_flags;
    std::optional<std::vector<uint32_t>> m_ordinals;
};

}
