// FBZZ Engine
// DX12StateTracker.cpp | fbzz::renderer
// バインド地点で必要な Transition Barrier を溜め込み、まとめて発行する
#include "DX12StateTracker.hpp"

namespace fbzz::renderer {

void DX12StateTracker::Register(ID3D12Resource* resource, D3D12_RESOURCE_STATES initialState)
{
    if (resource) m_states[resource] = initialState;
}

void DX12StateTracker::Remove(ID3D12Resource* resource)
{
    m_states.erase(resource);
}

void DX12StateTracker::Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                                  D3D12_RESOURCE_STATES requiredState)
{
    if (!commands || !resource) return;
    QueueTransition(resource, requiredState);
    FlushBarriers(commands);
}

void DX12StateTracker::QueueTransition(ID3D12Resource* resource, D3D12_RESOURCE_STATES requiredState)
{
    if (!resource) return;
    const auto found = m_states.find(resource);
    const D3D12_RESOURCE_STATES current = found != m_states.end()
        ? found->second : D3D12_RESOURCE_STATE_COMMON;
    if (current == requiredState) return;

    // 同じ Flush までに A→B→C と要求される場合、B への遷移を別バリアとして残すと
    // D3D12 は同一 ResourceBarrier 呼び出し内の重複サブリソース遷移として警告する。
    // GPU は Flush 前にはまだ遷移していないため、最初の StateBefore から最後の
    // StateAfter へ 1 本に畳み込めば記録順と実際の状態が一致する。
    for (auto it = m_pendingBarriers.begin(); it != m_pendingBarriers.end(); ++it) {
        if (it->Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
            it->Transition.pResource != resource ||
            it->Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
            continue;

        it->Transition.StateAfter = requiredState;
        if (it->Transition.StateBefore == requiredState)
            m_pendingBarriers.erase(it);
        m_states[resource] = requiredState;
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = current;
    barrier.Transition.StateAfter = requiredState;
    // WHY: 同一バッチ内で同じリソースが複数回 Queue されても、m_states を即座に更新しておけば
    //      次の呼び出しは "既に requiredState" として自然に重複除去される。
    m_pendingBarriers.push_back(barrier);
    m_states[resource] = requiredState;
}

void DX12StateTracker::FlushBarriers(ID3D12GraphicsCommandList* commands)
{
    if (!commands || m_pendingBarriers.empty()) return;
    commands->ResourceBarrier(static_cast<UINT>(m_pendingBarriers.size()), m_pendingBarriers.data());
    m_pendingBarriers.clear();
}

void DX12StateTracker::Clear()
{
    m_states.clear();
    m_pendingBarriers.clear();
}

} // namespace fbzz::renderer
