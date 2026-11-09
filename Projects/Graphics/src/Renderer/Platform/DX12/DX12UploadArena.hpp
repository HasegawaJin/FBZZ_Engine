/// @file    DX12UploadArena.hpp
/// @brief   フレームごとの一時 GPU Upload メモリを線形割り当てするアリーナ。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once
#include <Graphics/Api.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

namespace fbzz::renderer {

class DX12UploadArena final {
public:
    static constexpr uint32_t FRAME_COUNT = 2;

    struct Allocation {
        uint8_t* cpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        size_t size = 0;
        explicit operator bool() const { return cpu != nullptr; }
    };

    FBZZ_GRAPHICS_API ~DX12UploadArena();
    FBZZ_GRAPHICS_API bool Initialize(ID3D12Device* device, size_t bytesPerFrame);
    FBZZ_GRAPHICS_API void Shutdown();
    FBZZ_GRAPHICS_API void BeginFrame(uint32_t frameIndex);
    FBZZ_GRAPHICS_API Allocation Allocate(size_t sizeBytes, size_t alignment);
    /// @pre data は sizeBytes バイト読み取り可能で、destination は COPY_DEST 状態であること。
    /// @return 空きがあれば転送元を所有してコピーを記録する。容量不足なら何も記録せず false。
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copybufferregion CopyBufferRegion のバイト単位のオフセット。
    [[nodiscard]] bool TryStageCopy(ID3D12GraphicsCommandList& commands, ID3D12Resource& destination,
        size_t destinationOffset, const void* data, size_t sizeBytes);

    /// @note BeginFrame のたびに進む世代。アリーナはここでオフセットを 0 へ巻き戻すため、
    /// @note 以前に配った GPU アドレスはこの値が変わった時点で無効になる。
    /// @note 呼び出し側 (DX12ConstantBuffer) が「前回配られたアドレスをまだ使い回せるか」を判定するのに使う。
    /// @note       フレームインデックスは 2 つを往復するだけなので、同じ値に戻ったときに古いアドレスを
    /// @note       有効と誤認しないよう単調増加させる。
    uint64_t GetEpoch() const { return m_epoch; }

private:
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
