/// @file    UIPointer.hpp
/// @brief   UI のヒット判定に使うポインター位置の差し替え口
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// @note UIButton/UISlider の判定は Canvas 空間の座標 1 つと押下状態だけで決まるため、差し替え口を 1 つ用意すればウィジェット側を変えずにマウスとパッドを 1 本の経路へ寄せられる。
/// @note 常設グローバルではなく毎フレーム宣言にするのは、保持者が消えた後 (シーン遷移・Play 停止) に古い座標が残って UI が触れない/勝手に押される状態になるのを避けるため。途切れたら自動で OS のマウスへ戻る。
#pragma once

#include <Math/Vector2.hpp>
#include <cstdint>

namespace fbzz::scene {

class UIPointer {
public:
    /// このフレームの UI ポインターを宣言する。毎フレーム呼ぶこと。
    /// @param canvasPosition Canvas 空間 (左上原点) の座標
    /// @param pressed        押下中か (マウス左 / パッド A などを畳んだ結果)
    static void Set(const math::Vector2& canvasPosition, bool pressed);

    /// 明示的に取り下げる。次のフレームから OS のマウスへ戻る。
    static void Clear();

    /// 差し替えが生きているか。最後の Set から 1 フレームを過ぎると false になる。
    [[nodiscard]] static bool IsActive();

    [[nodiscard]] static math::Vector2 Position();
    [[nodiscard]] static bool Pressed();

private:
    static math::Vector2 s_position;
    static bool          s_pressed;
    static uint64_t      s_frame;      ///< 最後に Set したフレーム。0 = 未設定
};

} // namespace fbzz::scene
