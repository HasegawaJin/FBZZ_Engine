/// @file    IBuffer.hpp
/// @brief   GPU バッファの抽象インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note 具体バックエンドのバッファ実装を隠し、サイズや stride だけを公開する。
/// @note 生成は ResourceManager 経由に限定する。
#pragma once
#include <cstddef>
#include <cstdint>

/// @note INVALID_BINDLESS_INDEX の正本。
#include "ITexture.hpp"

namespace fbzz::renderer
{
    class IBuffer
    {
    public:
        virtual ~IBuffer() = default;
        
        /// @brief CPU 側からデータを再書き込み (動的バッファ用)。
        virtual void Update(const void* data, size_t sizeBytes) = 0;

        virtual size_t GetSize() const = 0;
        /// @brief 頂点バッファのみ有効。インデックスバッファでは 0 を返す。
        virtual std::uint32_t GetStride() const = 0;

        /// @brief GPU 書き込み可能な頂点バッファ (コンピュートスキニングの出力) の UAV 添字。
        /// @return 書き込み経路を持たないバッファは INVALID_BINDLESS_INDEX。
        /// @see Docs/design/bindless.md
        virtual std::uint32_t GetBindlessUavIndex() const { return INVALID_BINDLESS_INDEX; }

        /// @return ByteAddressBuffer SRV の添字。未対応・未初期化・読取り snapshot 確保失敗時は INVALID_BINDLESS_INDEX。
        /// @note Raw SRV は有効なデータ長が 4 byte の倍数であることを要求する。
        /// @note CPU 更新後は新しい添字へ差し替わり得る。使用前に引き直し、GPU table も内容版と合わせる。
        virtual std::uint32_t GetBindlessSrvIndex() const { return INVALID_BINDLESS_INDEX; }

        /// @return 記録済み内容の単調な版。0 は版追跡未対応、または未初期化の GPU 出力。
        virtual std::uint64_t GetContentVersion() const { return 0; }

        /// @note DIRECT queue に書込みを記録したバックエンドだけが呼ぶ。内容版は後続の AS と同じ順序で公開する。
        virtual void NotifyGpuWrite() {}

        /// @return 最新の CPU 内容をコピーできれば true。失敗時は output を変更しない。
        /// @note GPU 専用の内容は読み戻さない。呼出元が Update と同じスレッドで使用する。
        virtual bool CopyData(size_t, size_t, void*) const { return false; }
    };

} /// @note namespace fbzz::renderer
