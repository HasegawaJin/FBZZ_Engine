// FBZZ Engine
// DX12ConstantBuffer.hpp | fbzz::renderer
// Submit 時点の GPU 仮想アドレスを保持する DirectX 12 定数バッファ
#pragma once

#include <Engine/Renderer/IConstantBuffer.hpp>
#include <d3d12.h>
#include <vector>

namespace fbzz::renderer {

class DX12UploadArena;

class DX12ConstantBuffer final : public IConstantBuffer {
public:
    bool Init(DX12UploadArena* arena, size_t sizeBytes);
    void Update(const void* data, size_t sizeBytes) override;
    size_t GetSize() const override { return m_size; }
    // 現在のフレーム用UploadArenaへCPU側の最新値を転送し、Root CBVアドレスを返す。
    D3D12_GPU_VIRTUAL_ADDRESS PrepareForSubmit();

private:
    DX12UploadArena* m_arena = nullptr;
    size_t m_size = 0;
    std::vector<uint8_t> m_cpuData;
    bool m_hasData = false;

    // 同一フレーム内で内容が変わっていなければ、前回配られたスライスを使い回す。
    // WHY: Submit ごとに束縛中の全 CB を再確保 + 全域 memcpy していたため、Camera/Light/Shadow の
    //      ようにパス全体で不変な CB まで毎ドロー転送していた (GBuffer で約 1.8KB/draw)。
    //      値が変わっていないなら GPU 側のスライスもそのまま有効なので、アドレスだけ返せばよい。
    // NOTE: アリーナはフレーム開始でオフセットを巻き戻すため、必ず世代を照合すること。
    D3D12_GPU_VIRTUAL_ADDRESS m_cachedGpuAddress = 0;
    uint64_t m_cachedArenaEpoch = 0;
    bool m_dirty = true;
};

} // namespace fbzz::renderer
