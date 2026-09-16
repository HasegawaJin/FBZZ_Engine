/// @file    DX12StateTracker.hpp
/// @brief   DirectX 12 リソース状態を追跡して必要な遷移バリアだけを記録する。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

#include <d3d12.h>
#include <unordered_map>
#include <vector>

namespace fbzz::renderer {

class DX12StateTracker final {
public:
    void Register(ID3D12Resource* resource, D3D12_RESOURCE_STATES initialState);
    void Remove(ID3D12Resource* resource);

    // 単発の遷移。QueueTransition + FlushBarriers を即座に行う (1 リソース = 1 ResourceBarrier 呼び出し)。
    void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES requiredState);

    // 複数リソースの遷移を溜め込み、FlushBarriers でまとめて 1 回の ResourceBarrier 呼び出しに一括化する。
    // WHY: Submit/Dispatch/SetRenderTarget はバインド地点ごとに複数リソースを遷移させるが、
    //      1 件ずつ ResourceBarrier を発行すると draw/dispatch 回数分の API 呼び出しが積み重なる。
    void QueueTransition(ID3D12Resource* resource, D3D12_RESOURCE_STATES requiredState);
    void FlushBarriers(ID3D12GraphicsCommandList* commands);

    void Clear();

private:
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> m_states;
    std::vector<D3D12_RESOURCE_BARRIER> m_pendingBarriers;
};

} // namespace fbzz::renderer
