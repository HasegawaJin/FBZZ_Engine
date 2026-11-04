/// @file    BufferReadScope.hpp
/// @brief   同期バッファ読取りの境界を共有するスコープ。描画方式には依存しない。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <cstdint>
#include <span>
namespace fbzz::renderer::buffer_state_detail {
/// @note Caller owns the manager and exact facade throughout this synchronous scope; nested scopes retain legacy drains.
/// @note Only the drain boundary is reused. Every read resolves current handles and native values, including writes through previously borrowed IBuffer pointers.
class BufferStateReadScope final {
public:
    FBZZ_GRAPHICS_API BufferStateReadScope(ResourceManager& resources, IRenderer& expectedFacade) noexcept;
    FBZZ_GRAPHICS_API ~BufferStateReadScope();
    /// @return True when returned native values have a current idle witness; false retains the ordinary full-drain result.
    FBZZ_GRAPHICS_API bool Read(std::span<const ResourceHandle<BufferTag>> handles, std::span<BufferResourceState> out);
    /// @note Same native read position and borrowed-pointer contract as ResourceManager::Get; only a proven idle drain is omitted.
    [[nodiscard]] FBZZ_GRAPHICS_API IBuffer* Get(ResourceHandle<BufferTag> handle);
    /// @note This attests only to the drain boundary; it never proves that previously returned native values remain unchanged.
    [[nodiscard]] FBZZ_GRAPHICS_API bool HasCurrentReadBoundary() const;
    BufferStateReadScope(const BufferStateReadScope&) = delete;
    BufferStateReadScope& operator=(const BufferStateReadScope&) = delete;
private:
    friend class fbzz::renderer::ResourceManager;
    FBZZ_GRAPHICS_API void CaptureReadBoundary();
    ResourceManager& m_resources;
    IRenderer& m_facade;
    BufferStateReadScope* m_previous = nullptr;
    uint64_t m_instance = 0, m_epoch = 0, m_frame = 0, m_submission = 0, m_revision = 0;
    bool m_armed = false;
};
} /// @note namespace fbzz::renderer::buffer_state_detail
