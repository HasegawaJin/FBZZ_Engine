/// @file    HotkeyScope.hpp
/// @brief   キーが効く «面» の識別子。パネル・EditorContext・HotkeyManager の共通語。
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// @note パネル (IPanel) や EditorContext はこの enum だけを必要とし、キーの登録簿までは要らない。
///       基底クラスがマネージャー一式を抱えると、パネルを 1 つ足すたびに再コンパイルされる。
#pragma once
#include <cstdint>

namespace fbzz::editor {

/// ホットキーが効く文脈。ビットマスクなので複数指定できる (例: Delete は Scene View と Hierarchy の両方で効く)。
/// @note フォーカス位置を条件に含めないと 1 箇所へ集約できない。Scene View の W (ギズモ) と
///       カメラ移動の W の衝突も、この概念が無かったために起きていた。
enum class HotkeyScope : std::uint32_t {
    None          = 0,
    Global        = 1u << 0,   ///< フォーカス位置によらず有効
    SceneViewport = 1u << 1,
    Hierarchy     = 1u << 2,
    AssetBrowser  = 1u << 3,
    FluidEditor   = 1u << 4,   ///< Fluid Editor パネルにフォーカスがあるとき
};

constexpr HotkeyScope operator|(HotkeyScope a, HotkeyScope b)
{
    return static_cast<HotkeyScope>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr bool HasScope(HotkeyScope set, HotkeyScope test)
{
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(test)) != 0;
}

} // namespace fbzz::editor
