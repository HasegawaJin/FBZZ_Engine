/// @file    WorldBroadPhaseDebug.cpp
/// @brief   ブロードフェーズのデバッグ要求と結果のスナップショット。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/World/World.hpp>
#include <algorithm>
#include <atomic>

namespace fbzz::physics {

uint64_t World::NextDebugWorldEpoch()
{
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

void World::SetBroadPhaseDebugRequest(const BroadPhaseDebugRequest& request)
{
    m_broadPhaseDebugRequest = request;
    auto& bounded = m_broadPhaseDebugRequest;
    bounded.enabled &= bounded.hierarchy || bounded.bounds || bounded.candidatePairs;
    bounded.maxDepth = std::min(bounded.maxDepth, BROAD_PHASE_DEBUG_MAX_DEPTH);
    bounded.minDepth = std::min(bounded.minDepth, bounded.maxDepth);
    bounded.maxNodes = std::min(bounded.maxNodes, BROAD_PHASE_DEBUG_MAX_ITEMS);
    bounded.maxProxies = std::min(bounded.maxProxies, BROAD_PHASE_DEBUG_MAX_ITEMS);
    bounded.maxPairs = std::min(bounded.maxPairs, BROAD_PHASE_DEBUG_MAX_ITEMS);
    m_broadPhaseDebugSnapshot.reset();
    m_pendingBroadPhaseDebugSnapshot.reset();
}

void World::CaptureBroadPhaseDebugSnapshot()
{
    if (!m_broadPhaseDebugRequest.enabled) {
        return;
    }
    auto snapshot = std::make_shared<BroadPhaseDebugSnapshot>();
    snapshot->worldEpoch = m_debugEpoch.value;
    snapshot->sequence = ++m_broadPhaseDebugSequence;
    snapshot->request = m_broadPhaseDebugRequest;
    snapshot->selectionValid = !snapshot->request.selectedOnly;
    std::size_t selectedIndex = SIZE_MAX;

    // 別の World や再利用されたスロットを誤って選ばないよう、世代と登録状態を検証してから入力の添字を探す。
    if (snapshot->request.selectedOnly) {
        const ColliderHandle selected = snapshot->request.selected;
        if (snapshot->request.selectedWorldEpoch == m_debugEpoch.value && selected.IsValid()
            && selected.slot <= m_colliderPool.size()) {
            const auto& slot = m_colliderPool[static_cast<std::size_t>(selected.slot - 1)];
            if (slot.occupied && slot.generation == selected.generation) {
                const std::size_t limit = std::min(m_colliders.size(), BROAD_PHASE_DEBUG_MAX_WORK);
                for (std::size_t index = 0; index < limit; ++index) {
                    if (m_colliders[index].collider == slot.collider.collider) {
                        selectedIndex = index;
                        snapshot->selectionValid = true;
                        break;
                    }
                }
                snapshot->workLimitReached = selectedIndex == SIZE_MAX && limit != m_colliders.size();
            }
        }
    }
    m_solver.CaptureBroadPhaseDebugSnapshot(snapshot->request, m_colliders, m_collisionPairs,
        selectedIndex, *snapshot);

    // 採取した添字だけを要求リストにし、登録プールを上限まで走査してハンドルへ変換する。
    std::map<std::size_t, ColliderHandle> handles;
    for (const auto& proxy : snapshot->proxies) {
        handles.emplace(proxy.sourceIndex, ColliderHandle{});
    }
    for (const auto& pair : snapshot->pairs) {
        handles.emplace(pair.sourceIndexA, ColliderHandle{});
        handles.emplace(pair.sourceIndexB, ColliderHandle{});
    }
    auto next = handles.begin();
    std::size_t sourceIndex = 0;
    std::size_t poolIndex = 0;
    for (; next != handles.end() && poolIndex < m_colliderPool.size()
        && poolIndex < BROAD_PHASE_DEBUG_MAX_WORK; ++poolIndex) {
        const auto& slot = m_colliderPool[poolIndex];
        if (!slot.occupied || !slot.collider.collider) {
            continue;
        }
        if (next->first == sourceIndex) {
            next->second = {static_cast<uint32_t>(poolIndex + 1), slot.generation};
            ++next;
        }
        ++sourceIndex;
    }
    if (next != handles.end()) {
        snapshot->workLimitReached = true;
    }
    for (auto& proxy : snapshot->proxies) {
        proxy.handle = handles[proxy.sourceIndex];
    }
    for (auto& pair : snapshot->pairs) {
        pair.handleA = handles[pair.sourceIndexA];
        pair.handleB = handles[pair.sourceIndexB];
    }
    // 走査上限で対応付けできなかった項目は除き、診断結果が切り詰められたことを残す。
    const auto proxyEnd = std::remove_if(snapshot->proxies.begin(), snapshot->proxies.end(),
        [](const BroadPhaseDebugProxy& proxy) { return !proxy.handle.IsValid(); });
    snapshot->proxiesTruncated |= proxyEnd != snapshot->proxies.end();
    snapshot->proxies.erase(proxyEnd, snapshot->proxies.end());
    const auto pairEnd = std::remove_if(snapshot->pairs.begin(), snapshot->pairs.end(),
        [](const BroadPhaseDebugPair& pair) { return !pair.handleA.IsValid() || !pair.handleB.IsValid(); });
    snapshot->pairsTruncated |= pairEnd != snapshot->pairs.end();
    snapshot->pairs.erase(pairEnd, snapshot->pairs.end());
    m_pendingBroadPhaseDebugSnapshot = std::move(snapshot);
}

} /// @note namespace fbzz::physics
