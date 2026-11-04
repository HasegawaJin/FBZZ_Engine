/// @file    CoroutineFrameLoop.hpp
/// @brief   Coroutine の既存待機経路で所有されたフレーム処理を進める内部部品。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Engine/Scene/Coroutine.hpp>
#include <functional>

namespace fbzz::scene::coroutine_detail {

/// @return step が false を返すと完了。初回実行は生成フレームより後。
/// @note callback はフレームが所有し、Script の停止・破棄時には既存 Coroutine とともに解放する。
[[nodiscard]] Coroutine RunFrames(std::function<bool()> step);

} /// @note namespace fbzz::scene::coroutine_detail
