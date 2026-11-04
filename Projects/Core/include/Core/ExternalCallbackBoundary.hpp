/// @file    ExternalCallbackBoundary.hpp
/// @brief   所有スレッドの外部通知前に保留した内部結果を確定する。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#pragma once
#include <Core/Api.hpp>

namespace fbzz::core {

/// @note 非所有の thread-local stack。外側から内側の順に同期通知し、スコープ外へ context を保持しない。
/// @pre handler は外部 callback・ログ・pump を呼ばず、失敗時も未完成の公開状態を無効化してから戻る。
/// @note native 実行は確保しない。通知時だけ必要な上位層の標準所有値コピーは許容する。
/// @note handler の false は内部結果の失敗であり、元の外部 callback を省略する指示ではない。
class ExternalCallbackScope final {
public:
    using Handler = bool (*)(void*);

    FBZZ_CORE_API ExternalCallbackScope(Handler handler, void* context) noexcept;
    FBZZ_CORE_API ~ExternalCallbackScope();
    ExternalCallbackScope(const ExternalCallbackScope&) = delete;
    ExternalCallbackScope& operator=(const ExternalCallbackScope&) = delete;

private:
    friend FBZZ_CORE_API bool NotifyBeforeExternalCallback() noexcept;
    FBZZ_CORE_API bool NotifyOuterFirst() noexcept;
    Handler m_handler = nullptr;
    void* m_context = nullptr;
    ExternalCallbackScope* m_previous = nullptr;
};

/// @return すべての handler が成功なら true。false でも各 handler は失敗状態の公開を完了している。
/// @note ロック取得前に呼ぶ。handler 自体の再入を抑止して未完成結果を隠す guard は設けない。
[[nodiscard]] FBZZ_CORE_API bool NotifyBeforeExternalCallback() noexcept;


}
