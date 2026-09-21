/// @file    DX12UploadArena.hpp
/// @brief   フレームごとの一時 GPU Upload メモリを線形割り当てするアリーナ。
/// @author  Hasegawa Jin
/// @date    2026-07-15
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

    /// @note BeginFrame のたびに進む世代。アリーナはここでオフセットを 0 へ巻き戻すため、
    /// @note 以前に配った GPU アドレスはこの値が変わった時点で無効になる。
    /// @note 呼び出し側 (DX12ConstantBuffer) が「前回配られたアドレスをまだ使い回せるか」を判定するのに使う。
    /// @note       フレームインデックスは 2 つを往復するだけなので、同じ値に戻ったときに古いアドレスを
    /// @note       有効と誤認しないよう単調増加させる。
    uint64_t GetEpoch() const { return m_epoch; }

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
    uint64_t m_epoch = 0;
};

} /// @note namespace fbzz::renderer
