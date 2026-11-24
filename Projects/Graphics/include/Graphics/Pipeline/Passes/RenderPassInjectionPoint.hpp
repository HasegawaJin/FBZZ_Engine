/// @file    RenderPassInjectionPoint.hpp
/// @brief   拡張描画パスの挿入位置。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#pragma once
#include <cstdint>

namespace fbzz::renderer {

/// @note UserRenderPassInjectionPoint — Script が追加するパスを既存パイプラインのどこへ挿入するかを表す。
/// @note RenderGraph は依存関係で実行順を決めるが、HDR へ ReadWrite する透明系パスは依存が同じになりやすい。
/// @note 明示的な挿入点を持たせ、Water / VFX / PostProcess 前処理の意図をコードから読めるようにする。
enum class UserRenderPassInjectionPoint : uint8_t {
    AfterOpaque,
    AfterTransparent,
    BeforePostProcess
};

} /// @note namespace fbzz::renderer
