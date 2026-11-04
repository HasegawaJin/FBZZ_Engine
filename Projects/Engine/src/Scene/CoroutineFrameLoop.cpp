/// @file    CoroutineFrameLoop.cpp
/// @brief   フレーム待機の中断構文を Coroutine の内部実装へ閉じる。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include "CoroutineFrameLoop.hpp"

namespace fbzz::scene::coroutine_detail {

Coroutine RunFrames(std::function<bool()> step)
{
    if (!step) co_return;
    while (true) {
        co_await WaitForFrames(1);
        if (!step()) co_return;
    }
}

} /// @note namespace fbzz::scene::coroutine_detail
