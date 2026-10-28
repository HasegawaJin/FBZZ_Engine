/// @file    BufferStateReadScope.hpp
/// @brief   Bounded native buffer reads that reuse only an idle CPU drain boundary.
/// @author  Hasegawa Jin
/// @date    2026-10-16
#pragma once
#include <Graphics/Renderer/ResourceManager.hpp>
#include <cstdint>
#include <span>

#if defined(_WIN32) && defined(FBZZGraphics_EXPORTS)
#define FBZZ_BUFFER_STATE_READ_API __declspec(dllexport)
#elif defined(_WIN32)
#define FBZZ_BUFFER_STATE_READ_API __declspec(dllimport)
#else
#define FBZZ_BUFFER_STATE_READ_API
#endif

namespace fbzz::renderer::buffer_state_detail {

/// @note Exact registered producer only; busy, queued, executing or nested synchronous work fails without pumping.
[[nodiscard]] bool TryObserveIdleReadBoundary(IRenderer& renderer, uint64_t& submission, uint64_t& revision);

/// @note Caller owns the manager and exact facade throughout this synchronous scope; nested scopes retain legacy drains.
/// @note Only the drain boundary is reused. Every read resolves current handles and native values, including writes through previously borrowed IBuffer pointers.
class BufferStateReadScope final {
public:
    FBZZ_BUFFER_STATE_READ_API BufferStateReadScope(ResourceManager& resources, IRenderer& expectedFacade) noexcept;
    FBZZ_BUFFER_STATE_READ_API ~BufferStateReadScope();
    /// @return True when returned native values have a current idle witness; false retains the ordinary full-drain result.
    FBZZ_BUFFER_STATE_READ_API bool Read(std::span<const ResourceHandle<BufferTag>> handles, std::span<BufferResourceState> out);
    /// @note Same native read position and borrowed-pointer contract as ResourceManager::Get; only a proven idle drain is omitted.
    [[nodiscard]] FBZZ_BUFFER_STATE_READ_API IBuffer* Get(ResourceHandle<BufferTag> handle);
    /// @note This attests only to the drain boundary; it never proves that previously returned native values remain unchanged.
    [[nodiscard]] FBZZ_BUFFER_STATE_READ_API bool HasCurrentReadBoundary() const;
    BufferStateReadScope(const BufferStateReadScope&) = delete;
    BufferStateReadScope& operator=(const BufferStateReadScope&) = delete;
private:
    friend class fbzz::renderer::ResourceManager;
    void CaptureReadBoundary();
    ResourceManager& m_resources;
    IRenderer& m_facade;
    BufferStateReadScope* m_previous = nullptr;
    uint64_t m_instance = 0, m_epoch = 0, m_frame = 0, m_submission = 0, m_revision = 0;
    bool m_armed = false;
};
} /// @note namespace fbzz::renderer::buffer_state_detail
#undef FBZZ_BUFFER_STATE_READ_API
