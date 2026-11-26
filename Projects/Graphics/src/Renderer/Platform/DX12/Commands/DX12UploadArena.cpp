/// @file    DX12UploadArena.cpp
/// @brief   256 byte CBV アラインメントを含む Upload Heap の線形割り当て。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#include "DX12UploadArena.hpp"

#include <Core/Logger/Logger.hpp>
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <utility>

namespace fbzz::renderer {

DX12UploadArena::~DX12UploadArena() { Shutdown(); }

bool DX12UploadArena::Initialize(ID3D12Device* device, size_t bytesPerFrame, size_t maxBytesPerFrame)
{
    if (!device || m_device || bytesPerFrame == 0) {
        return false;
    }
    if (maxBytesPerFrame < bytesPerFrame || maxBytesPerFrame > MAX_BYTES_PER_FRAME) {
        return false;
    }

    // 最初のページだけをスロットごとに確保し、全ページを使える状態になってから初期化を終える。
    m_device = device;
    m_pageBytes = bytesPerFrame;
    m_maxBytesPerFrame = maxBytesPerFrame;
    for (FrameArena& frame : m_frames) {
        if (!CreatePage(frame.pages.front(), bytesPerFrame)) {
            FBZZ_LOG_ERROR("DX12UploadArena: %zu byte の初期ページの確保に失敗しました", bytesPerFrame);
            Shutdown();
            return false;
        }
        frame.usage.residentPages = 1;
        frame.usage.committedBytes = bytesPerFrame;
    }
    return true;
}

bool DX12UploadArena::CreatePage(Page& page, size_t bytes) const
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // ドライバーやマップの失敗時に、既存ページや使用量を変更しない。
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource)))) {
        return false;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS gpu = resource->GetGPUVirtualAddress();
    if (gpu == 0 || bytes > (std::numeric_limits<D3D12_GPU_VIRTUAL_ADDRESS>::max)() - gpu) {
        return false;
    }
    const D3D12_RANGE cpuReadRange{0, 0};
    void* mapped = nullptr;
    if (FAILED(resource->Map(0, &cpuReadRange, &mapped))) {
        return false;
    }
    if (!mapped) {
        resource->Unmap(0, nullptr);
        return false;
    }

    page = {std::move(resource), static_cast<uint8_t*>(mapped), gpu, bytes, 0};
    return true;
}

void DX12UploadArena::ReleasePage(Page& page)
{
    page.resource->Unmap(0, nullptr);
    page = {};
}

void DX12UploadArena::Shutdown()
{
    for (FrameArena& frame : m_frames) {
        for (size_t pageIndex = 0; pageIndex < frame.usage.residentPages; ++pageIndex) {
            ReleasePage(frame.pages[pageIndex]);
        }
        frame.usage = {};
        frame.failureLogged = false;
    }
    if (m_device) {
        ++m_epoch;
    }
    m_device.Reset();
    m_pageBytes = 0;
    m_maxBytesPerFrame = 0;
    m_currentFrame = 0;
}

void DX12UploadArena::BeginFrame(uint32_t frameIndex)
{
    m_currentFrame = frameIndex % FRAME_COUNT;
    FrameArena& frame = m_frames[m_currentFrame];
    // 呼び出し元がこのスロットのフェンスを待っているため、すべてのページを再利用できる。
    for (size_t pageIndex = 0; pageIndex < frame.usage.residentPages; ++pageIndex) {
        frame.pages[pageIndex].offset = 0;
    }
    frame.usage.pagesUsed = 0;
    frame.usage.usedBytes = 0;
    frame.usage.failedAllocations = 0;
    frame.usage.failedRequestBytes = 0;
    frame.usage.largestFailedRequestBytes = 0;
    frame.failureLogged = false;
    ++m_epoch;
}

DX12UploadArena::Allocation DX12UploadArena::Allocate(size_t sizeBytes, size_t alignment)
{
    return Allocate(sizeBytes, alignment, true);
}

DX12UploadArena::Allocation DX12UploadArena::Allocate(size_t sizeBytes, size_t alignment, bool logFailure)
{
    if (!m_device) {
        return {};
    }

    FrameArena& frame = m_frames[m_currentFrame];
    const Allocation allocation = TryAllocate(frame, sizeBytes, alignment);
    if (allocation) {
        return allocation;
    }

    // フォールバックするバッファー転送も集計するが、ログは直接割り当ての失敗時に各フレーム一度だけ出す。
    constexpr size_t MAX_COUNTER = (std::numeric_limits<size_t>::max)();
    if (frame.usage.failedAllocations != MAX_COUNTER) {
        ++frame.usage.failedAllocations;
    }
    frame.usage.failedRequestBytes += (std::min)(sizeBytes, MAX_COUNTER - frame.usage.failedRequestBytes);
    frame.usage.largestFailedRequestBytes = (std::max)(frame.usage.largestFailedRequestBytes, sizeBytes);
    if (logFailure && !frame.failureLogged) {
        frame.failureLogged = true;
        FBZZ_LOG_ERROR("DX12UploadArena: %zu byte の要求に失敗しました (使用 %zu / 保持 %zu / 上限 %zu byte)",
            sizeBytes, frame.usage.usedBytes, frame.usage.committedBytes, m_maxBytesPerFrame);
    }
    return {};
}

DX12UploadArena::Allocation DX12UploadArena::AllocateFromPage(FrameArena& frame, Page& page,
    size_t sizeBytes, size_t alignment)
{
    // ページ先頭からの差分を先に検査して、整列と端位置の加算をページ内に収める。
    const size_t alignmentMask = alignment - 1;
    const size_t misalignment = (page.gpu + page.offset) & alignmentMask;
    const size_t alignmentPadding = (alignment - misalignment) & alignmentMask;
    const size_t available = page.capacity - page.offset;
    if (alignmentPadding > available || sizeBytes > available - alignmentPadding) {
        return {};
    }

    if (page.offset == 0) {
        ++frame.usage.pagesUsed;
    }
    const size_t alignedOffset = page.offset + alignmentPadding;
    page.offset = alignedOffset + sizeBytes;
    frame.usage.usedBytes += alignmentPadding + sizeBytes;
    frame.usage.peakBytesUsed = (std::max)(frame.usage.peakBytesUsed, frame.usage.usedBytes);
    return {page.mapped + alignedOffset, page.gpu + alignedOffset, sizeBytes, page.resource.Get(), alignedOffset};
}

bool DX12UploadArena::FreeUnusedPages(FrameArena& frame, size_t sizeBytes)
{
    size_t available = m_maxBytesPerFrame - frame.usage.committedBytes;
    if (available >= sizeBytes) {
        return true;
    }

    // 最初のページと今フレームに参照を渡したページは保持する。回収しても要求が入らなければ保持状態を変えない。
    size_t reclaimable = 0;
    for (size_t pageIndex = 1; pageIndex < frame.usage.residentPages; ++pageIndex) {
        const Page& page = frame.pages[pageIndex];
        if (page.offset == 0) {
            reclaimable += page.capacity;
        }
    }
    if (reclaimable < sizeBytes - available) {
        return false;
    }

    size_t pageIndex = 1;
    while (available < sizeBytes) {
        Page& page = frame.pages[pageIndex];
        if (page.offset != 0) {
            ++pageIndex;
            continue;
        }
        frame.usage.committedBytes -= page.capacity;
        available += page.capacity;
        ReleasePage(page);
        const size_t last = --frame.usage.residentPages;
        if (pageIndex != last) {
            page = std::move(frame.pages[last]);
            frame.pages[last] = {};
        }
    }
    return true;
}

DX12UploadArena::Allocation DX12UploadArena::TryAllocate(FrameArena& frame, size_t sizeBytes, size_t alignment)
{
    if (sizeBytes == 0 || sizeBytes > m_maxBytesPerFrame) {
        return {};
    }
    if (!std::has_single_bit(alignment) || alignment > D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) {
        return {};
    }

    // 最初のページから試し、過去または今フレームに確保した追加ページの空きも使う。
    for (size_t pageIndex = 0; pageIndex < frame.usage.residentPages; ++pageIndex) {
        if (const auto allocation = AllocateFromPage(frame, frame.pages[pageIndex], sizeBytes, alignment)) {
            return allocation;
        }
    }
    if (!FreeUnusedPages(frame, sizeBytes) || frame.usage.residentPages == frame.pages.size()) {
        return {};
    }

    // 大きな要求は専用ページに載せる。小さな初期設定でも、ページ数の上限内で容量を使える大きさにする。
    const size_t available = m_maxBytesPerFrame - frame.usage.committedBytes;
    const size_t minimumSpillBytes = (m_maxBytesPerFrame + MAX_SPILL_PAGES - 1) / MAX_SPILL_PAGES;
    const size_t spillBytes = (std::min)(available, (std::max)({m_pageBytes, minimumSpillBytes, sizeBytes}));
    Page& page = frame.pages[frame.usage.residentPages];
    if (!CreatePage(page, spillBytes)) {
        return {};
    }
    const auto allocation = AllocateFromPage(frame, page, sizeBytes, alignment);
    if (!allocation) {
        ReleasePage(page);
        return {};
    }
    ++frame.usage.residentPages;
    frame.usage.committedBytes += spillBytes;
    return allocation;
}

bool DX12UploadArena::TryStageCopy(ID3D12GraphicsCommandList& commands, ID3D12Resource& destination,
    size_t destinationOffset, const void* data, size_t sizeBytes)
{
    if (!data || sizeBytes == 0) {
        return false;
    }
    const auto destinationDesc = destination.GetDesc();
    if (destinationDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || destinationOffset > destinationDesc.Width
        || sizeBytes > destinationDesc.Width - destinationOffset) {
        return false;
    }

    // 転送を記録したページを、呼び出し元が同じスロットのフェンスを待つまで保持する。
    const auto allocation = Allocate(sizeBytes, 1, false);
    if (!allocation) {
        return false;
    }

    std::memcpy(allocation.cpu, data, sizeBytes);
    commands.CopyBufferRegion(&destination, destinationOffset, allocation.resource, allocation.offset, sizeBytes);
    return true;
}

DX12UploadArena::FrameUsage DX12UploadArena::GetFrameUsage(uint32_t frameIndex) const
{
    if (frameIndex >= FRAME_COUNT) {
        return {};
    }
    return m_frames[frameIndex].usage;
}

} // namespace fbzz::renderer
