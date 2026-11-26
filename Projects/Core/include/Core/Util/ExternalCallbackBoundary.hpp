/// @file    ExternalCallbackBoundary.hpp
/// @brief   所有スレッドの外部通知前に保留した内部結果を確定する。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#pragma once
#include <Core/Api.hpp>

namespace fbzz::core {

/// @note スレッドごとの非所有スタックを外側から内側へ同期通知する。コンテキストの参照はスコープ内に限る。
/// @pre ハンドラーは外部コールバック、ログ出力、メッセージ処理を呼ばず、失敗時も公開前の結果を無効化してから戻ること。
/// @note ネイティブ側の通常経路ではメモリを確保しない。通知に必要な場合に限り、上位層で所有する値をコピーできる。
/// @note ハンドラーが false を返しても、呼び出し元は元の外部コールバックを実行する。
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
    Handler m_handler = nullptr; ///< 外部コールバックの直前に呼ぶ内部通知関数。
    void* m_context = nullptr; ///< 通知関数に渡す非所有のコンテキスト。
    ExternalCallbackScope* m_previous = nullptr; ///< 同じスレッドの直前の外側スコープ。なければ nullptr。
};

/// @return 全ハンドラーが成功すれば true。失敗したハンドラーも、戻る前に失敗状態を公開する。
/// @note ロックの取得前に呼ぶ。ハンドラーの再入を抑止して未確定の状態を隠す仕組みは設けない。
[[nodiscard]] FBZZ_CORE_API bool NotifyBeforeExternalCallback() noexcept;


}
