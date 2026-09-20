/// @file    AssetLeaseSet.cpp
/// @brief   利用権の束。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#include <Engine/Asset/AssetLeaseSet.hpp>

#include <utility>

namespace fbzz::asset {

AssetLeaseSet::AssetLeaseSet(AssetLeaseSet&& other) noexcept
    : m_streamer(other.m_streamer), m_leases(std::move(other.m_leases))
{
    other.m_leases.clear();
}

AssetLeaseSet& AssetLeaseSet::operator=(AssetLeaseSet&& other) noexcept
{
    if (this != &other) {
        Release();
        m_streamer = other.m_streamer;
        m_leases = std::move(other.m_leases);
        other.m_leases.clear();
    }
    return *this;
}

AssetLoadError AssetLeaseSet::Add(std::string_view typeName, const std::string& reference,
                                  const AssetRequestOptions& options)
{
    if (!m_streamer) return AssetLoadError::NoChannel;
    const AssetStreamer::RawRequest request = m_streamer->RequestRaw(typeName, reference, options);
    if (request.leaseId == 0) return request.error;
    m_leases.push_back({ request.leaseId, std::string(typeName), request.handle });
    return AssetLoadError::None;
}

void AssetLeaseSet::SetPriority(AssetPriority priority)
{
    if (!m_streamer) return;
    for (const Held& held : m_leases) m_streamer->SetLeasePriority(held.leaseId, priority);
}

std::uint32_t AssetLeaseSet::CompleteAllNow()
{
    if (!m_streamer) return 0;
    std::uint32_t completed = 0;
    for (const Held& held : m_leases) {
        const AssetLoadState state = m_streamer->GetLeaseStatus(held.leaseId).state;
        if (state == AssetLoadState::Ready || state == AssetLoadState::Failed) continue;
        if (m_streamer->CompleteNowRaw(held.typeName, held.handle)) ++completed;
    }
    return completed;
}

AssetLeaseSet::Progress AssetLeaseSet::GetProgress() const
{
    Progress progress;
    if (!m_streamer) return progress;
    for (const Held& held : m_leases) {
        const AssetLoadState state = m_streamer->GetLeaseStatus(held.leaseId).state;
        /// @note 状態を引けない (プロジェクト切り替えで台帳から消えた) ものは待っても来ないので失敗に数える。
        ++progress.total;
        if (state == AssetLoadState::Ready) ++progress.ready;
        else if (state == AssetLoadState::Failed || state == AssetLoadState::Canceled
                 || state == AssetLoadState::None) ++progress.failed;
    }
    return progress;
}

void AssetLeaseSet::Release()
{
    if (m_streamer)
        for (const Held& held : m_leases) m_streamer->ReleaseLease(held.leaseId);
    m_leases.clear();
}

} // namespace fbzz::asset
