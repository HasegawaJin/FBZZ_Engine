/// @file    DX12UploadArena.cpp
/// @brief   256 byte CBV アラインメントを含む Upload Heap の線形割り当て。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#include "DX12UploadArena.hpp"

#include <Engine/Core/Logger.hpp>

namespace fbzz::renderer {

DX12UploadArena::~DX12UploadArena() { Shutdown(); }

bool DX12UploadArena::Initialize(ID3D12Device* device, size_t bytesPerFrame)
{
    if (!device || bytesPerFrame == 0)
        return false;
    m_capacity = bytesPerFrame;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytesPerFrame;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (FrameArena& frame : m_frames) {
        const HRESULT result = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&frame.resource));
        void* mapped = nullptr;
        if (FAILED(result) || FAILED(frame.resource->Map(0, nullptr, &mapped))) {
            FBZZ_LOG_ERROR("DX12UploadArena: %zu byte の Upload Heap 生成に失敗しました", bytesPerFrame);
            Shutdown();
            return false;
        }
        frame.mapped = static_cast<uint8_t*>(mapped);
    }
    return true;
}

void DX12UploadArena::Shutdown()
{
    for (FrameArena& frame : m_frames) {
        if (frame.resource && frame.mapped)
            frame.resource->Unmap(0, nullptr);
        frame.mapped = nullptr;
        frame.offset = 0;
        frame.resource.Reset();
    }
    m_capacity = 0;
}

void DX12UploadArena::BeginFrame(uint32_t frameIndex)
{
    m_currentFrame = frameIndex % FRAME_COUNT;
    m_frames[m_currentFrame].offset = 0;
    /// @note ここで配り直しが始まる = 既に配ったアドレスは無効。世代を進めて呼び出し側へ知らせる。
    ++m_epoch;
}

DX12UploadArena::Allocation DX12UploadArena::Allocate(size_t sizeBytes, size_t alignment)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0)
        return {};
    FrameArena& frame = m_frames[m_currentFrame];
    const size_t alignedOffset = (frame.offset + alignment - 1) & ~(alignment - 1);
    if (sizeBytes > m_capacity || alignedOffset > m_capacity - sizeBytes) {
        FBZZ_LOG_ERROR("DX12UploadArena: フレーム領域が不足しました (%zu / %zu bytes)",
                       alignedOffset + sizeBytes, m_capacity);
        return {};
    }
    frame.offset = alignedOffset + sizeBytes;
    return {frame.mapped + alignedOffset,
            frame.resource->GetGPUVirtualAddress() + alignedOffset, sizeBytes};
}

} // namespace fbzz::renderer
