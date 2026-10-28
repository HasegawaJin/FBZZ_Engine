/// @file    PreparedSkinning.cpp
/// @brief   所有済み計算列の公開寿命と登録済み native factory の選択を扱う。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#include <Graphics/Effects/PreparedSkinning.hpp>
#include "../../Renderer/PreparedSkinningBackend.hpp"
#include <Graphics/Renderer/ResourceManager.hpp>
#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace fbzz::renderer {
namespace prepared_skinning_detail {
namespace {
struct BackendEntry { IRenderer* native = nullptr; NativeFactory factory = nullptr; };
struct BackendRegistry { std::mutex mutex; std::array<BackendEntry, 16> entries; };
BackendRegistry& Backends() { static BackendRegistry registry; return registry; }
struct OwnerRegistry { std::mutex mutex; std::array<std::weak_ptr<ResourceOwner>, 64> entries; };
OwnerRegistry& Owners() { static OwnerRegistry registry; return registry; }
} /// @note namespace

std::shared_ptr<ResourceOwner> AcquireResourceOwner(const ResourceManager& resources)
{
    auto& registry = Owners();
    std::lock_guard lock(registry.mutex);
    std::weak_ptr<ResourceOwner>* empty = nullptr;
    for (auto& entry : registry.entries) {
        auto owner = entry.lock();
        if (owner && owner->instance == resources.GetInstanceId() && owner->epoch == resources.GetResetVersion()
            && owner->alive.load(std::memory_order_acquire)) return owner;
        if ((!owner || !owner->alive.load(std::memory_order_acquire)) && !empty) empty = &entry;
    }
    if (!empty) return {};
    auto owner = std::make_shared<ResourceOwner>();
    owner->instance = resources.GetInstanceId(); owner->epoch = resources.GetResetVersion();
    *empty = owner;
    return owner;
}

void InvalidateResourceOwner(uint64_t instance)
{
    auto& registry = Owners();
    std::lock_guard lock(registry.mutex);
    for (auto& entry : registry.entries)
        if (auto owner = entry.lock(); owner && owner->instance == instance)
            owner->alive.store(false, std::memory_order_release);
}

void RegisterBackend(IRenderer& native, NativeFactory factory)
{
    auto& registry = Backends();
    std::lock_guard lock(registry.mutex);
    for (auto& entry : registry.entries)
        if (entry.native == &native || !entry.native) { entry = {&native, factory}; return; }
}

void UnregisterBackend(IRenderer& native)
{
    auto& registry = Backends();
    std::lock_guard lock(registry.mutex);
    for (auto& entry : registry.entries) if (entry.native == &native) entry = {};
}

bool HasRegisteredBackend(const IRenderer& native) noexcept
{
    auto& registry = Backends();
    std::lock_guard lock(registry.mutex);
    for (const auto& entry : registry.entries) if (entry.native == &native) return entry.factory != nullptr;
    return false;
}

std::unique_ptr<NativeWork> PrepareExactNative(IRenderer& native, ResourceManager& resources, ResourceHandle<ShaderTag> shader,
    std::span<const CapturedSkinningRequest> requests, std::span<const math::Matrix4> matrices)
{
    NativeFactory factory = nullptr;
    {
        auto& registry = Backends();
        std::lock_guard lock(registry.mutex);
        for (const auto& entry : registry.entries) if (entry.native == &native) { factory = entry.factory; break; }
    }
    return factory ? factory(native, resources, shader, requests, matrices) : nullptr;
}

std::shared_ptr<PreparedSkinningTicket> TicketFactory::Make(std::unique_ptr<NativeWork> native)
{
    if (!native) return {};
    return std::make_shared<PreparedSkinningTicket>(PreparedSkinningTicket::ConstructionKey{}, std::move(native));
}
bool TicketFactory::MatchesFacade(const ResourceManager& resources, const IRenderer& facade)
{
    return &resources.m_renderer == &facade && !resources.m_rendererDetached;
}
} /// @note namespace prepared_skinning_detail

PreparedSkinningTicket::PreparedSkinningTicket(ConstructionKey, std::unique_ptr<prepared_skinning_detail::NativeWork> native)
    : m_native(std::move(native)) {}
PreparedSkinningTicket::~PreparedSkinningTicket() = default;
bool PreparedSkinningTicket::Submit() noexcept { return m_native && m_native->Submit(); }
bool SubmitPreparedSkinning(const std::shared_ptr<PreparedSkinningTicket>& ticket) noexcept { return ticket && ticket->Submit(); }
std::span<const PreparedSkinningWrite> PreparedSkinningTicket::Writes() const noexcept { return m_native->writes; }
bool PreparedSkinningTicket::IsWriteCurrent(uint32_t row) const noexcept { return m_native && m_native->OutputResource(row) != nullptr; }
uint64_t PreparedSkinningTicket::ResourceInstance() const noexcept { return m_native->instance; }
uint64_t PreparedSkinningTicket::ResourceEpoch() const noexcept { return m_native->epoch; }
uint64_t PreparedSkinningTicket::ResourceFrame() const noexcept { return m_native->frame; }

bool GpuSkinningDemandEnabled() noexcept
{
    const char* value = std::getenv("FBZZ_GPU_SKINNING_DEMAND");
    return value && std::strcmp(value, "1") == 0;
}

bool SupportsCapturedSkinning(const IRenderer& renderer, const ResourceManager& resources) noexcept
{
    return prepared_skinning_detail::TicketFactory::MatchesFacade(resources, renderer)
        && prepared_skinning_detail::SupportsViaFacade(renderer);
}

std::shared_ptr<PreparedSkinningTicket> PrepareCapturedSkinning(IRenderer& renderer, ResourceManager& resources,
    ResourceHandle<ShaderTag> shader, std::span<const CapturedSkinningRequest> requests, std::span<const math::Matrix4> matrices)
{
    if (!GpuSkinningDemandEnabled() || !prepared_skinning_detail::TicketFactory::MatchesFacade(resources, renderer)
        || requests.empty() || requests.size() > 256 || matrices.empty() || matrices.size() > 65536)
        return {};
    const std::vector<CapturedSkinningRequest> ownedRequests(requests.begin(), requests.end());
    const std::vector<math::Matrix4> ownedMatrices(matrices.begin(), matrices.end());
    auto native = prepared_skinning_detail::PrepareViaFacade(renderer, resources, shader, ownedRequests, ownedMatrices);
    return prepared_skinning_detail::TicketFactory::Make(std::move(native));
}
} /// @note namespace fbzz::renderer
