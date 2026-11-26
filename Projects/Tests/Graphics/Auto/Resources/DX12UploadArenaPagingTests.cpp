/// @file    DX12UploadArenaPagingTests.cpp
/// @brief   DX12 Upload ページのコピー元、上限、フェンス完了後の再利用を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-25
#include <TestKit/TestKit.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dxgi1_4.h>
#include "../../../../Graphics/src/Renderer/Platform/DX12/Commands/DX12UploadArena.hpp"

#include <array>
#include <cstring>
#include <memory>

namespace fbzz::tests {
namespace {

using Microsoft::WRL::ComPtr;

class DX12UploadArenaPagingTest : public testkit::Fixture {};

/// @brief 追加ページに置いたコピー元は上限エラー後も生存し、軽いフレームは先頭ページだけを使う。
TEST_F(DX12UploadArenaPagingTest, CappedCopiesPreserveEarlierPagesAndLightReuseUsesOnlyHead)
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    ASSERT_TRUE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    ASSERT_TRUE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))));
    ASSERT_TRUE(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))));

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    ASSERT_TRUE(SUCCEEDED(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue))));
    ASSERT_TRUE(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
    ASSERT_TRUE(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator.Get(), nullptr, IID_PPV_ARGS(&commands))));
    ASSERT_TRUE(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    const std::unique_ptr<void, decltype(&CloseHandle)> completed(CreateEventW(nullptr, FALSE, FALSE, nullptr), &CloseHandle);
    ASSERT_TRUE(completed);
    constexpr uint64_t SUBMITTED_FENCE = 1;
    ASSERT_TRUE(SUCCEEDED(fence->SetEventOnCompletion(SUBMITTED_FENCE, completed.get())));

    constexpr size_t PAGE_BYTES = 64u * 1024u;
    constexpr size_t CAP_BYTES = 2 * PAGE_BYTES;
    constexpr size_t PREFIX_BYTES = 256;
    constexpr std::array<uint32_t, 5> EXPECTED{0x10203040u, 0x30405060u, 0x50607080u, 0x708090A0u, 0x90A0B0C0u};
    constexpr size_t READBACK_SIZE = PAGE_BYTES + sizeof(EXPECTED);
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = READBACK_SIZE;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    ASSERT_TRUE(SUCCEEDED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))));

    // 先頭ページのコピーを先に記録し、追加ページの非ゼロオフセットにも異なる値を置く。
    renderer::DX12UploadArena arena;
    ASSERT_TRUE(arena.Initialize(device.Get(), PAGE_BYTES, CAP_BYTES));
    arena.BeginFrame(0);
    const uint64_t firstEpoch = arena.GetEpoch();
    const auto head = arena.Allocate(PAGE_BYTES, PREFIX_BYTES);
    ASSERT_TRUE(head);
    std::memcpy(head.cpu, &EXPECTED[0], sizeof(uint32_t));
    std::memcpy(head.cpu + sizeof(uint32_t), &EXPECTED[3], sizeof(uint32_t));
    commands->CopyBufferRegion(readback.Get(), 0, head.resource, head.offset, sizeof(uint32_t));
    commands->CopyBufferRegion(readback.Get(), 3 * sizeof(uint32_t), head.resource, head.offset + sizeof(uint32_t), sizeof(uint32_t));

    const auto spill = arena.Allocate(PREFIX_BYTES, PREFIX_BYTES);
    ASSERT_TRUE(spill);
    ASSERT_NE(spill.resource, head.resource);
    EXPECT_EQ(arena.GetEpoch(), firstEpoch);
    std::memcpy(spill.cpu, &EXPECTED[1], sizeof(uint32_t));
    commands->CopyBufferRegion(readback.Get(), sizeof(uint32_t), spill.resource, spill.offset, sizeof(uint32_t));
    ASSERT_TRUE(arena.TryStageCopy(*commands.Get(), *readback.Get(), 2 * sizeof(uint32_t), &EXPECTED[2], sizeof(uint32_t)));

    // 上限でコピーを拒否しても、先に書いた印と追加ページの残りは使える。
    const auto beforeFailure = arena.GetFrameUsage(0);
    const std::array<uint8_t, PAGE_BYTES> excess{};
    EXPECT_FALSE(arena.TryStageCopy(*commands.Get(), *readback.Get(), 3 * sizeof(uint32_t), excess.data(), excess.size()));
    const auto afterFailure = arena.GetFrameUsage(0);
    EXPECT_EQ(afterFailure.pagesUsed, 2u);
    EXPECT_EQ(afterFailure.committedBytes, CAP_BYTES);
    EXPECT_EQ(afterFailure.usedBytes, beforeFailure.usedBytes);
    EXPECT_EQ(afterFailure.failedAllocations, 1u);
    EXPECT_EQ(afterFailure.largestFailedRequestBytes, PAGE_BYTES);
    ASSERT_TRUE(arena.TryStageCopy(*commands.Get(), *readback.Get(), 4 * sizeof(uint32_t), &EXPECTED[4], sizeof(uint32_t)));

    ASSERT_TRUE(SUCCEEDED(commands->Close()));
    ID3D12CommandList* submitted[]{commands.Get()};
    queue->ExecuteCommandLists(1, submitted);
    ASSERT_TRUE(SUCCEEDED(queue->Signal(fence.Get(), SUBMITTED_FENCE)));

    // 他のスロットは独立して使える。同じスロットは GPU のフェンス完了後にだけ巻き戻す。
    arena.BeginFrame(1);
    const auto otherSlot = arena.Allocate(sizeof(uint32_t), PREFIX_BYTES);
    EXPECT_TRUE(otherSlot);
    EXPECT_NE(otherSlot.resource, head.resource);
    EXPECT_NE(otherSlot.resource, spill.resource);
    EXPECT_EQ(arena.GetFrameUsage(1).pagesUsed, 1u);
    constexpr DWORD FENCE_TIMEOUT_MS = 5000;
    ASSERT_EQ(WaitForSingleObject(completed.get(), FENCE_TIMEOUT_MS), WAIT_OBJECT_0);
    ASSERT_EQ(fence->GetCompletedValue(), SUBMITTED_FENCE);

    const D3D12_RANGE readRange{0, sizeof(EXPECTED)};
    void* mapped = nullptr;
    ASSERT_TRUE(SUCCEEDED(readback->Map(0, &readRange, &mapped)));
    std::array<uint32_t, EXPECTED.size()> values{};
    std::memcpy(values.data(), mapped, sizeof(values));
    constexpr D3D12_RANGE NO_CPU_WRITES{0, 0};
    readback->Unmap(0, &NO_CPU_WRITES);
    EXPECT_EQ(values, EXPECTED);

    arena.BeginFrame(0);
    const auto reused = arena.Allocate(sizeof(uint32_t), PREFIX_BYTES);
    ASSERT_TRUE(reused);
    EXPECT_EQ(reused.gpu, head.gpu);
    const auto light = arena.GetFrameUsage(0);
    EXPECT_EQ(light.pagesUsed, 1u);
    EXPECT_EQ(light.usedBytes, sizeof(uint32_t));
    EXPECT_EQ(light.failedAllocations, 0u);
    EXPECT_EQ(light.peakBytesUsed, beforeFailure.usedBytes + sizeof(uint32_t));
}

} // namespace
} // namespace fbzz::tests
