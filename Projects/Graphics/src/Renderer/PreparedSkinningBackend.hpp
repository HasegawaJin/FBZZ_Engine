/// @file    PreparedSkinningBackend.hpp
/// @brief   登録済み native backend だけが作成できる所有済み計算列の内部境界。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#pragma once
#include <Graphics/Effects/PreparedSkinning.hpp>
#include <vector>
#include <atomic>

namespace fbzz::renderer::prepared_skinning_detail {

struct BufferWriteVersion { std::atomic<uint64_t> value{0}; };
struct ResourceOwner {
    uint64_t instance = 0, epoch = 0;
    std::atomic<bool> alive{true};
};
std::shared_ptr<ResourceOwner> AcquireResourceOwner(const ResourceManager& resources);
void InvalidateResourceOwner(uint64_t instance);

class NativeWork {
public:
    virtual ~NativeWork() = default;
    virtual bool Submit() noexcept = 0;
    virtual const void* OutputResource(size_t row) const noexcept = 0;
    virtual uint64_t OutputAddress(size_t row) const noexcept = 0;
    virtual uint32_t OutputBytes(size_t row) const noexcept = 0;
    virtual uint32_t OutputSrv(size_t row) const noexcept = 0;
    virtual const void* DeviceIdentity() const noexcept = 0;
    uint64_t instance = 0, epoch = 0, frame = 0;
    std::vector<PreparedSkinningWrite> writes;
};

struct TicketFactory {
    static bool MatchesFacade(const ResourceManager& resources, const IRenderer& facade);
    static std::shared_ptr<PreparedSkinningTicket> Make(std::unique_ptr<NativeWork> native);
    static const NativeWork& Native(const PreparedSkinningTicket& ticket) { return *ticket.m_native; }
};

using NativeFactory = std::unique_ptr<NativeWork> (*)(IRenderer&, ResourceManager&, ResourceHandle<ShaderTag>,
    std::span<const CapturedSkinningRequest>, std::span<const math::Matrix4>);

/// @note 登録関数は Graphics DLL 内の final native 実装だけから呼ぶ。
void RegisterBackend(IRenderer& native, NativeFactory factory);
void UnregisterBackend(IRenderer& native);
bool HasRegisteredBackend(const IRenderer& native) noexcept;
bool SupportsViaFacade(const IRenderer& facade) noexcept;
std::unique_ptr<NativeWork> PrepareExactNative(IRenderer& native, ResourceManager& resources, ResourceHandle<ShaderTag> shader,
    std::span<const CapturedSkinningRequest> requests, std::span<const math::Matrix4> matrices);
std::unique_ptr<NativeWork> PrepareViaFacade(IRenderer& facade, ResourceManager& resources, ResourceHandle<ShaderTag> shader,
    std::span<const CapturedSkinningRequest> requests, std::span<const math::Matrix4> matrices);
} /// @note namespace fbzz::renderer::prepared_skinning_detail
