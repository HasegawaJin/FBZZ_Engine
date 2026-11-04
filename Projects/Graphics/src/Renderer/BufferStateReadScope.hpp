/// @file    BufferStateReadScope.hpp
/// @brief   Graphics 内部の読取り可能状態の検証。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#pragma once
#include <Graphics/Renderer/BufferReadScope.hpp>
namespace fbzz::renderer::buffer_state_detail {
/// @note 未登録・実行中・入れ子の producer は再利用可能な境界を持たない。
[[nodiscard]] FBZZ_GRAPHICS_API bool TryObserveIdleReadBoundary(IRenderer& renderer, uint64_t& submission, uint64_t& revision);
} /// @note namespace fbzz::renderer::buffer_state_detail
