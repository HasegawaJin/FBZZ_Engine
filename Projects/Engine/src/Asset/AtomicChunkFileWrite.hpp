/// @file    AtomicChunkFileWrite.hpp
/// @brief   Keep atomic chunk publication and owned-temporary cleanup independent of filesystem failure injection.
/// @author  Hasegawa Jin
/// @date    2026-10-20
#pragma once
#include <system_error>

namespace fbzz::asset::detail {

/// @note CRT failures retain both errno and the underlying Windows error before any close, cleanup or log callback.
struct AtomicWriteError {
    std::error_code code;
    unsigned long nativeCode = 0;

    [[nodiscard]] explicit operator bool() const { return static_cast<bool>(code) || nativeCode != 0; }
};

/// @note Each failed operation retains its own error; cleanup cannot overwrite the original publication failure.
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

/// @brief Publish only a fully written, flushed and closed exclusive temporary; otherwise clean only that owned path.
/// @note Io owns the exclusive temporary after a successful Open; failed Open never grants cleanup ownership.
/// @note No retry or callback occurs here. Production Io bounds name collisions and reports errors after this procedure.
/// @see Docs/design/asset-streaming.md «チャンク索引・部分 I/O»
template<typename Io, typename WritePayload>
[[nodiscard]] AtomicWriteResult WriteAtomicChunkFile(Io& io, WritePayload&& writePayload)
{
    AtomicWriteResult result;
    result.createParent = io.CreateParent();
    if (result.createParent) return result;
    result.open = io.Open();
    if (result.open) return result;

    result.write = writePayload(io);
    if (!result.write) result.flush = io.Flush();
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

} /// @note namespace fbzz::asset::detail
