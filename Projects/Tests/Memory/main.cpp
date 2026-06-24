// FBZZ Engine
// Tests/Memory/main.cpp
// Memory モジュール単体テスト: LinearAllocator / StackAllocator / PoolAllocator / FrameAllocator
// エンジン本体を起動せずにアロケータのライフサイクルと統計を検証する。
#include <cstdio>
#include <cstddef>

#include <Engine/Core/Memory/LinearAllocator.hpp>
#include <Engine/Core/Memory/StackAllocator.hpp>
#include <Engine/Core/Memory/PoolAllocator.hpp>
#include <Engine/Core/Memory/FrameAllocator.hpp>

#include "../TestHelper.hpp"

using namespace fbzz::core;

// ─── LinearAllocator ──────────────────────────────────────────────────────────

static void TestMemory_Linear()
{
    std::printf("\n=== Memory: LinearAllocator ===\n");

    LinearAllocator alloc;
    check(alloc.Initialize(1024), "LinearAllocator: Initialize(1024) succeeds");
    check(alloc.IsInitialized(),  "LinearAllocator: IsInitialized() == true");

    // 1 回目の確保
    void* p1 = alloc.Allocate(64);
    check(p1 != nullptr,   "LinearAllocator: Allocate(64) returns non-null");
    check(alloc.Owns(p1),  "LinearAllocator: Owns(p1) == true");

    // 統計が更新される
    {
        const MemoryStats s = alloc.GetStats();
        check(s.used >= 64,          "LinearAllocator: stats.used >= 64");
        check(s.allocationCount == 1,"LinearAllocator: allocationCount == 1");
    }

    // 2 回目の確保 — 異なるアドレス
    void* p2 = alloc.Allocate(128);
    check(p2 != nullptr && p2 != p1, "LinearAllocator: 2nd Allocate returns different ptr");

    // Free は no-op — used は変わらない
    const std::size_t usedBefore = alloc.GetStats().used;
    alloc.Free(p1);
    check(alloc.GetStats().used == usedBefore, "LinearAllocator: Free is no-op (used unchanged)");

    // Reset で先頭へ戻る
    alloc.Reset();
    check(alloc.GetStats().used == 0, "LinearAllocator: Reset() clears used to 0");

    // Reset 後に再確保できる
    void* p3 = alloc.Allocate(64);
    check(p3 != nullptr, "LinearAllocator: Allocate after Reset succeeds");

    // 容量を超えると nullptr
    alloc.Reset();
    void* big = alloc.Allocate(2048);
    check(big == nullptr, "LinearAllocator: Allocate beyond capacity returns nullptr");

    // CreateObject / DestroyObject でプレースメント構築・破棄できる
    alloc.Reset();
    struct Vec3Tmp { float x, y, z; };
    Vec3Tmp* v = CreateObject<Vec3Tmp>(alloc, 1.0f, 2.0f, 3.0f);
    check(v != nullptr,          "LinearAllocator: CreateObject constructs object");
    check(v->x == 1.0f && v->z == 3.0f, "LinearAllocator: CreateObject sets fields");
    DestroyObject<Vec3Tmp>(alloc, v); // デストラクタ呼び出し + Free(no-op)

    alloc.Shutdown();
    check(!alloc.IsInitialized(), "LinearAllocator: Shutdown() clears initialized state");
}

// ─── StackAllocator ───────────────────────────────────────────────────────────

static void TestMemory_Stack()
{
    std::printf("\n=== Memory: StackAllocator ===\n");

    StackAllocator alloc;
    check(alloc.Initialize(1024), "StackAllocator: Initialize(1024) succeeds");

    void* a = alloc.Allocate(64);
    void* b = alloc.Allocate(64);
    check(a != nullptr && b != nullptr, "StackAllocator: two Allocates succeed");
    check(alloc.Owns(a) && alloc.Owns(b), "StackAllocator: Owns returns true for both");

    // 統計: 2 回 Allocate
    check(alloc.GetStats().allocationCount == 2, "StackAllocator: allocationCount == 2");
    check(alloc.GetStats().activeCount     == 2, "StackAllocator: activeCount == 2");

    // LIFO Free — b を解放すると activeCount が減る
    alloc.Free(b);
    check(alloc.GetStats().activeCount == 1, "StackAllocator: activeCount == 1 after Free(b)");

    // b の解放後に同サイズを再確保できる
    void* c = alloc.Allocate(64);
    check(c != nullptr, "StackAllocator: Allocate after Free(b) succeeds");

    // Reset で全解放
    alloc.Reset();
    check(alloc.GetStats().used == 0, "StackAllocator: Reset() clears used to 0");

    // Reset 後に再確保できる
    void* d = alloc.Allocate(64);
    check(d != nullptr, "StackAllocator: Allocate after Reset succeeds");

    // 容量を超えると nullptr
    alloc.Reset();
    void* big = alloc.Allocate(2048);
    check(big == nullptr, "StackAllocator: Allocate beyond capacity returns nullptr");

    alloc.Shutdown();
    check(!alloc.IsInitialized(), "StackAllocator: Shutdown() clears initialized state");
}

// ─── PoolAllocator ────────────────────────────────────────────────────────────

static void TestMemory_Pool()
{
    std::printf("\n=== Memory: PoolAllocator ===\n");

    constexpr std::size_t BLOCK_SIZE  = 32;
    constexpr std::size_t BLOCK_COUNT = 4;

    PoolAllocator alloc;
    check(alloc.Initialize(BLOCK_SIZE, BLOCK_COUNT), "PoolAllocator: Initialize(32, 4) succeeds");
    check(alloc.BlockSize()  == BLOCK_SIZE,  "PoolAllocator: BlockSize() == 32");
    check(alloc.BlockCount() == BLOCK_COUNT, "PoolAllocator: BlockCount() == 4");

    // BLOCK_COUNT 個全て確保できる
    void* ptrs[BLOCK_COUNT];
    for (std::size_t i = 0; i < BLOCK_COUNT; ++i)
    {
        ptrs[i] = alloc.AllocateBlock();
        check(ptrs[i] != nullptr,        "PoolAllocator: AllocateBlock() returns non-null");
        check(alloc.Owns(ptrs[i]),        "PoolAllocator: Owns returns true for allocated block");
    }

    // プール満杯 → nullptr
    void* overflow = alloc.AllocateBlock();
    check(overflow == nullptr, "PoolAllocator: AllocateBlock() returns nullptr when full");

    // 1 つ Free すると再確保できる
    alloc.Free(ptrs[0]);
    void* reused = alloc.AllocateBlock();
    check(reused != nullptr, "PoolAllocator: AllocateBlock() succeeds after Free");

    // Reset → 全ブロック再利用可能
    alloc.Reset();
    {
        bool allSucceed = true;
        for (std::size_t i = 0; i < BLOCK_COUNT; ++i)
        {
            if (alloc.AllocateBlock() == nullptr) { allSucceed = false; break; }
        }
        check(allSucceed, "PoolAllocator: all blocks available after Reset");
    }

    // Allocate(size) 呼び出しも機能する
    alloc.Reset();
    void* p = alloc.Allocate(BLOCK_SIZE);
    check(p != nullptr, "PoolAllocator: Allocate(blockSize) succeeds");

    alloc.Shutdown();
    check(!alloc.IsInitialized(), "PoolAllocator: Shutdown() clears initialized state");
}

// ─── FrameAllocator ───────────────────────────────────────────────────────────

static void TestMemory_Frame()
{
    std::printf("\n=== Memory: FrameAllocator ===\n");

    FrameAllocator alloc;
    check(alloc.Initialize(1024), "FrameAllocator: Initialize(1024) succeeds");

    // フレーム 1 — 確保 → EndFrame で used が 0 に戻る
    {
        alloc.BeginFrame();
        void* p = alloc.Allocate(128);
        check(p != nullptr,                  "FrameAllocator: Allocate in frame 1 succeeds");
        check(alloc.Owns(p),                 "FrameAllocator: Owns returns true during frame");
        check(alloc.GetStats().used >= 128,  "FrameAllocator: stats.used >= 128 after alloc");
        alloc.EndFrame();
        check(alloc.GetStats().used == 0,    "FrameAllocator: EndFrame() resets used to 0");
    }

    // フレーム 2 — 同じ容量で再確保できる
    {
        alloc.BeginFrame();
        void* p = alloc.Allocate(128);
        check(p != nullptr, "FrameAllocator: Allocate in frame 2 succeeds");
        alloc.EndFrame();
    }

    // 1 フレーム内で複数確保
    {
        alloc.BeginFrame();
        void* pa = alloc.Allocate(64);
        void* pb = alloc.Allocate(64);
        void* pc = alloc.Allocate(64);
        check(pa != nullptr && pb != nullptr && pc != nullptr,
              "FrameAllocator: multiple allocs in one frame succeed");
        alloc.EndFrame();
        check(alloc.GetStats().used == 0, "FrameAllocator: EndFrame resets after multiple allocs");
    }

    // 容量を超えると nullptr
    {
        alloc.BeginFrame();
        void* big = alloc.Allocate(2048);
        check(big == nullptr, "FrameAllocator: Allocate beyond capacity returns nullptr");
        alloc.EndFrame();
    }

    alloc.Shutdown();
    check(!alloc.IsInitialized(), "FrameAllocator: Shutdown() clears initialized state");
}

// ─── エントリポイント ─────────────────────────────────────────────────────────

int main()
{
    std::printf("FBZZ Memory Tests\n");
    std::printf("=================\n");

    TestMemory_Linear();
    TestMemory_Stack();
    TestMemory_Pool();
    TestMemory_Frame();

    std::printf("\n=================\n");
    std::printf("Results: %d passed, %d failed\n", g_passed, g_failed);

    return g_failed == 0 ? 0 : 1;
}
