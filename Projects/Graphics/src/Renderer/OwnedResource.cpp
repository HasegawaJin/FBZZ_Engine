/// @file    OwnedResource.cpp
/// @brief   所有ハンドルを現在の Active ではなく作成元の Manager へ返す。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <Graphics/Renderer/OwnedResource.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
namespace fbzz::renderer {
namespace {
template<class Tag>
void ReleaseOwner(ResourceHandle<Tag> handle)
{
    if (handle.IsValid())
        if (auto* owner = ResourceManager::FindOwner(handle.owner)) owner->Release(handle);
}
} /// @note namespace
void ReleaseOwnedResource(ResourceHandle<TextureTag> handle) { ReleaseOwner(handle); }
void ReleaseOwnedResource(ResourceHandle<BufferTag> handle) { ReleaseOwner(handle); }
void ReleaseOwnedResource(ResourceHandle<ConstantBufferTag> handle) { ReleaseOwner(handle); }
void ReleaseOwnedResource(ResourceHandle<StructuredBufferTag> handle) { ReleaseOwner(handle); }
void ReleaseOwnedResource(ResourceHandle<RenderTargetTag> handle) { ReleaseOwner(handle); }
void ReleaseOwnedResource(ResourceHandle<PipelineStateTag> handle) { ReleaseOwner(handle); }
} /// @note namespace fbzz::renderer
