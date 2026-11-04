/// @file    OwnedResource.hpp
/// @brief   作成元へ返却する GPU 資源の move 専用所有ハンドル。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <utility>

namespace fbzz::renderer {
/// @note 終了済みの所有者と失効済みの世代には何もしない。資源管理スレッドで呼ぶ。
FBZZ_GRAPHICS_API void ReleaseOwnedResource(ResourceHandle<TextureTag> handle);
FBZZ_GRAPHICS_API void ReleaseOwnedResource(ResourceHandle<BufferTag> handle);
FBZZ_GRAPHICS_API void ReleaseOwnedResource(ResourceHandle<ConstantBufferTag> handle);
FBZZ_GRAPHICS_API void ReleaseOwnedResource(ResourceHandle<StructuredBufferTag> handle);
FBZZ_GRAPHICS_API void ReleaseOwnedResource(ResourceHandle<RenderTargetTag> handle);
FBZZ_GRAPHICS_API void ReleaseOwnedResource(ResourceHandle<PipelineStateTag> handle);

/// @note Create 系の単独所有結果だけを受け取る。Load 系の共有キャッシュとプール借用品には使わない。
template<class Tag>
class OwnedResource {
public:
    OwnedResource() = default;
    OwnedResource(ResourceHandle<Tag> handle) : m_handle(handle) {}
    ~OwnedResource() { Reset(); }
    OwnedResource(const OwnedResource&) = delete;
    OwnedResource& operator=(const OwnedResource&) = delete;
    OwnedResource(OwnedResource&& other) noexcept : m_handle(std::exchange(other.m_handle, {})) {}
    OwnedResource& operator=(OwnedResource&& other) noexcept
    {
        if (this != &other) { Reset(); m_handle = std::exchange(other.m_handle, {}); }
        return *this;
    }
    /// @note 戻り値と暗黙変換は非所有参照。所有権は移さない。
    [[nodiscard]] ResourceHandle<Tag> Get() const { return m_handle; }
    /// @note 所有権を呼出し側へ渡す。以後この owner からは返却しない。
    [[nodiscard]] ResourceHandle<Tag> Release() { return std::exchange(m_handle, {}); }
    operator ResourceHandle<Tag>() const { return Get(); }
    [[nodiscard]] bool IsValid() const { return m_handle.IsValid(); }
    void Reset(ResourceHandle<Tag> replacement = {})
    {
        if (m_handle == replacement) return;
        ReleaseOwnedResource(m_handle);
        m_handle = replacement;
    }
private:
    ResourceHandle<Tag> m_handle;
};
} /// @note namespace fbzz::renderer
