// FBZZ Engine
// DX12UploadArena.hpp | fbzz::renderer
// フレームごとの一時 GPU Upload メモリを線形割り当てするアリーナ
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

namespace fbzz::renderer {

class DX12UploadArena final {
public:
    struct Allocation {
        uint8_t* cpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        size_t size = 0;
        explicit operator bool() const { return cpu != nullptr; }
    };

    ~DX12UploadArena();
    bool Initialize(ID3D12Device* device, size_t bytesPerFrame);
    void Shutdown();
    void BeginFrame(uint32_t frameIndex);
    Allocation Allocate(size_t sizeBytes, size_t alignment);

private:
    static constexpr uint32_t FRAME_COUNT = 2;
    struct FrameArena {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint8_t* mapped = nullptr;
        size_t offset = 0;
    };

    std::array<FrameArena, FRAME_COUNT> m_frames;
    size_t m_capacity = 0;
    uint32_t m_currentFrame = 0;
};

} // namespace fbzz::renderer
