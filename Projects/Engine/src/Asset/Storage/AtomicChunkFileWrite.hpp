/// @file    AtomicChunkFileWrite.hpp
/// @brief   チャンクの原子的な公開と、自分が所有する一時ファイルの後始末をファイルシステム障害注入から分離する。
/// @author  Hasegawa Jin
/// @date    2026-10-20
#pragma once
#include <system_error>

namespace fbzz::asset::detail {

/// @note CRT の失敗時は閉じる処理、後始末、ログのコールバックより前に errno と Windows 側の元のエラーを保存する。
struct AtomicWriteError {
    std::error_code code;
    unsigned long nativeCode = 0;

    [[nodiscard]] explicit operator bool() const { return static_cast<bool>(code) || nativeCode != 0; }
};

/// @note 各操作の失敗にはそれぞれのエラーを保持する。後始末の失敗で公開時の元のエラーを上書きしない。
struct AtomicWriteResult {
    AtomicWriteError createParent;
    AtomicWriteError open;
    AtomicWriteError write;
    AtomicWriteError flush;
    AtomicWriteError close;
    AtomicWriteError publish;
    AtomicWriteError cleanup;
    bool published = false;
    bool cleanupAttempted = false;
};

/// @brief 排他的に作成した一時ファイルへの書き込み・フラッシュ・クローズが完了してから公開する。失敗時はそのパスだけを後始末する。
/// @note Io が排他的な一時ファイルの所有権を得るのは Open の成功後。Open に失敗した場合は後始末しない。
/// @note ここでは再試行やコールバックを行わない。本番の Io が名前衝突の再試行回数を制限し、この処理後にエラーを報告する。
/// @see Docs/design/asset-streaming.md «チャンク索引・部分 I/O»
template<typename Io, typename WritePayload>
[[nodiscard]] AtomicWriteResult WriteAtomicChunkFile(Io& io, WritePayload&& writePayload)
{
    AtomicWriteResult result;
    result.createParent = io.CreateParent();
    if (result.createParent) {
        return result;
    }
    result.open = io.Open();
    if (result.open) {
        return result;
    }

    result.write = writePayload(io);
    if (!result.write) {
        result.flush = io.Flush();
    }
    result.close = io.Close();
    if (!result.write && !result.flush && !result.close) {
        result.publish = io.Publish();
        if (!result.publish) {
            result.published = true;
            return result;
        }
    }
    result.cleanupAttempted = true;
    result.cleanup = io.Cleanup();
    return result;
}

} // namespace fbzz::asset::detail
