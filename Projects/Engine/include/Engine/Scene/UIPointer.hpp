/// @file    UIPointer.hpp
/// @brief   UI のヒット判定に使うポインター位置の差し替え口
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// 設計意図 (WHY):
///   UIButton / UISlider の判定は「Canvas 空間の座標 1 つと押下状態」だけで決まる。
///   つまりゲーム内カーソルを作るのに必要なのは、その 1 つの入力を差し替える口だけで、
///   ウィジェット側は何も変えなくていい。マウスとパッドを 1 本の経路へ寄せられる。
///
/// WHY 常設のグローバルにせず「毎フレーム更新」にするか:
///   置きっぱなしにすると、カーソルを持つスクリプトが消えた後 (シーン遷移・Play 停止)
///   に古い座標が残り、UI が触れない or 勝手に押される状態になる。原因が
///   「前のシーンの残骸」なので追いにくい。書き手が毎フレーム宣言し、
///   途切れたら自動で OS のマウスへ戻る形にする。
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
