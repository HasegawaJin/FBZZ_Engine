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

    /// @note 単発の遷移。QueueTransition + FlushBarriers を即座に行う (1 リソース = 1 ResourceBarrier 呼び出し)。
    void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES requiredState);

    /// @note 複数リソースの遷移を溜め込み、FlushBarriers でまとめて 1 回の ResourceBarrier 呼び出しに一括化する。
    /// @note Submit/Dispatch/SetRenderTarget はバインド地点ごとに複数リソースを遷移させるが、
    /// @note       1 件ずつ発行すると draw/dispatch 回数分の API 呼び出しが積み重なるため。
    void QueueTransition(ID3D12Resource* resource, D3D12_RESOURCE_STATES requiredState);
    void FlushBarriers(ID3D12GraphicsCommandList* commands);

    /// @brief 追跡中のリソースをすべて COMMON へ移す遷移を溜める。
    /// @note 非同期コンピュート区間へ入る前に、描画キュー側で呼ぶ。COMPUTE キューは
    /// @note       PIXEL_SHADER_RESOURCE / RENDER_TARGET / DEPTH_* を扱えず、その状態のまま渡すと
    /// @note       コンピュート側で遷移も使用もできない。COMMON はどのキューでも合法で、そこからなら
    /// @note       UNORDERED_ACCESS / NON_PIXEL_SHADER_RESOURCE へ自由に移せる。
    /// @note 「区間が触るリソースだけ」を宣言させないのは、宣言漏れが «たまに壊れる» 形でしか
    /// @note       現れず、絵から原因に辿りつけないため。
    /// @see Docs/design/async-compute.md §3
    void QueueTransitionAllToCommon();

    void Clear();

private:
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> m_states;
    std::vector<D3D12_RESOURCE_BARRIER> m_pendingBarriers;
};

} /// @note namespace fbzz::renderer
