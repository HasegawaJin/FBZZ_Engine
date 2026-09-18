/// @file    ScriptMemoryProxy.hpp
/// @brief   Script から一時メモリ / 固定長プールを安全に扱うための Proxy。
/// @author  Hasegawa Jin
/// @date    2026-06-01
#pragma once

#include <Engine/Core/Memory/Allocator.hpp>
#include <Engine/Core/Memory/FrameAllocator.hpp>
#include <Engine/Core/Memory/PoolAllocator.hpp>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace fbzz::scene {

class Script;

/// @brief Script 用のローカルアロケータを公開する。
/// @note core::Allocator の詳細を露出しすぎず、new/delete 禁止ルールを保ったまま
///       短命データと固定長データを高速に扱えるようにする。
struct ScriptMemoryProxy {
    /// @note 所有元 Script への非所有参照を受け取るだけで、Script 本体は所有しない。
    ScriptMemoryProxy() = default;
    explicit ScriptMemoryProxy(Script* owner) : script(owner) {}

    Script* script = nullptr;

    /// @brief Script 専用のフレーム一時領域を明示的に確保する。
    /// @note 既定容量で足りない Script が、OnAwake などで用途に合う容量へ調整できる。
    [[nodiscard]] bool InitializeFrame(std::size_t capacity) const;

    /// @brief 現在フレームだけ有効な一時メモリを確保する。
    /// @note 未初期化なら DEFAULT_FRAME_CAPACITY で遅延初期化し、毎フレーム
    ///       ScriptSystem::Update 時に Reset される。
    [[nodiscard]] void* AllocateFrame(std::size_t size,
                                      std::size_t alignment = alignof(std::max_align_t)) const;

    /// @brief フレーム一時領域を手動で巻き戻す。
    /// @note OnUpdate 内で段階的に大きな一時バッファを使う Script が、明示的に
    ///       再利用点を作れるようにする。
    void ResetFrame() const;

    /// @brief Script 専用のフレーム一時領域を解放する。
    void ShutdownFrame() const;

    /// @brief 固定サイズ要素用の Script 専用プールを確保する。
    /// @note 弾丸や短命イベントなど、同じサイズを繰り返し生成/破棄する Script で
    ///       断片化と確保コストを抑える。
    [[nodiscard]] bool InitializePool(std::size_t blockSize,
                                      std::size_t blockCount,
                                      std::size_t alignment = alignof(std::max_align_t)) const;

    /// @brief T に必要なサイズ / アラインメントで固定長プールを初期化する。
    /// @note blockSize や alignment の指定ミスによる assert を避け、記述を型中心にする。
    template <class T>
    [[nodiscard]] bool InitializePoolFor(std::size_t blockCount) const
    {
        return InitializePool(sizeof(T), blockCount, alignof(T));
    }

    /// @brief 固定長プールから 1 ブロックを取得 / 返却する。
    /// @pre size は blockSize 以下、alignment は InitializePool の alignment 以下であること。
    [[nodiscard]] void* AllocatePool(std::size_t size,
                                     std::size_t alignment = alignof(std::max_align_t)) const;
    void FreePool(void* ptr) const;
    void ResetPool() const;
    void ShutdownPool() const;

    [[nodiscard]] bool IsFrameInitialized() const;
    [[nodiscard]] bool IsPoolInitialized() const;
    [[nodiscard]] fbzz::core::MemoryStats GetFrameStats() const;
    [[nodiscard]] fbzz::core::MemoryStats GetPoolStats() const;

    /// @brief フレーム一時領域上に T を構築する。
    /// @note フレーム Reset 時にデストラクタは呼ばれないため、破棄処理を必要としない
    ///       一時データに用途を限定する。
    template <class T, class... Args>
    [[nodiscard]] T* NewFrame(Args&&... args) const
    {
        static_assert(std::is_trivially_destructible_v<T>,
                      "ScriptMemoryProxy::NewFrame requires trivially destructible types.");

        void* memory = AllocateFrame(sizeof(T), alignof(T));
        if (memory == nullptr) {
            return nullptr;
        }

        return std::construct_at(static_cast<T*>(memory), std::forward<Args>(args)...);
    }

    /// @brief 固定長プール上に T を構築 / 破棄する。
    /// @note プールは個別 Free できるため、非 trivial な Script 専用オブジェクトも
    ///       安全に寿命を閉じられる。
    template <class T, class... Args>
    [[nodiscard]] T* NewPool(Args&&... args) const
    {
        if (!m_poolAllocator.IsInitialized() || sizeof(T) > m_poolAllocator.BlockSize()) {
            return nullptr;
        }

        return fbzz::core::CreateObject<T>(m_poolAllocator, std::forward<Args>(args)...);
    }

    template <class T>
    void DeletePool(T* ptr) const
    {
        if (ptr == nullptr || !m_poolAllocator.IsInitialized()) {
            return;
        }

        fbzz::core::DestroyObject(m_poolAllocator, ptr);
    }

    /// @brief ScriptSystem のフレーム境界から呼ばれる内部用入口。
    /// @note Script 作者が毎回 ResetFrame を呼ばなくても、フレーム一時領域の寿命を安定させる。
    void BeginFrame() const;

private:
    static constexpr std::size_t DEFAULT_FRAME_CAPACITY = 64u * 1024u;

    mutable fbzz::core::FrameAllocator m_frameAllocator;
    mutable fbzz::core::PoolAllocator  m_poolAllocator;
};

} // namespace fbzz::scene
