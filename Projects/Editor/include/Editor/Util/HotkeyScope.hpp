/// @file    HotkeyScope.hpp
/// @brief   キーが効く «面» の識別子。パネル・EditorContext・HotkeyManager の共通語。
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// WHY HotkeyManager.hpp から切り出すか:
///   この enum を名乗るのはパネル (IPanel) と EditorContext で、どちらも
///   «キーの登録簿» そのものは要らない。基底クラスがマネージャー一式を抱えると、
///   パネルを 1 つ足すたびにホットキー実装まで再コンパイルされる。
#pragma once
#include <cstdint>

namespace fbzz::editor {

// ホットキーが効く文脈。ビットマスクなので複数指定できる
// (例: Delete は Scene View と Hierarchy の両方で効く)。
//
// WHY: 同じキーがパネルごとに違う意味を持つ以上、「どこにフォーカスがあるか」を
//      条件に含めないと 1 箇所へ集約できない。Scene View の W (ギズモ) と
//      カメラ移動の W が衝突していたのも、この概念が無かったため。
enum class HotkeyScope : std::uint32_t {
    None          = 0,
    Global        = 1u << 0,   // フォーカス位置によらず有効
    SceneViewport = 1u << 1,
    Hierarchy     = 1u << 2,
    AssetBrowser  = 1u << 3,
    FluidEditor   = 1u << 4,   // Fluid Editor パネルにフォーカスがあるとき
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
