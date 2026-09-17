/// @file    Boss03AnimParams.hpp
/// @brief   Boss03.animcontroller のパラメーター名と、翼 6 枚の識別
/// @author  Hasegawa Jin
/// @date    2026-09-15
///
/// @note `SetFloat`/`SetTrigger` は綴りが違っても黙って無視するため、`.animcontroller` 側との突き合わせ箇所を 1 箇所に閉じる (`BossAnimParams.hpp` と同じ理由)。
/// @note ScriptCodeGen はスクリプト登録マクロ直前の最後の namespace 行をそのまま登録名に採る。`Boss03AnimatorComponent.hpp` に同居させ入れ子 namespace を書くと存在しない名前空間で登録され `ScriptList.inl` が壊れる。
#pragma once

#include <string_view>

namespace sandbox {

/// Boss03.animcontroller のパラメーター名。
namespace boss03anim {
inline constexpr const char* kMotion      = "Motion";      ///< 浮遊の向き (Boss03Motion の値)
inline constexpr const char* kIsClosed    = "IsClosed";    ///< 繭。true の間 Close → Cocoon_Idle
inline constexpr const char* kIsStaggered = "IsStaggered"; ///< 崩れて倒れている
inline constexpr const char* kIsDead      = "IsDead";      ///< 撃破
inline constexpr const char* kHit         = "Hit";         ///< 加算レイヤーの被弾リアクション
inline constexpr const char* kSlamL       = "Slam_L";      ///< 左翼の叩きつけ
inline constexpr const char* kSlamR       = "Slam_R";      ///< 右翼の叩きつけ
inline constexpr const char* kSlamCombo   = "Slam_Combo";  ///< 両翼の叩きつけ (締め)
inline constexpr const char* kLaserFan    = "Laser_Fan";   ///< 扇の焼き払い
inline constexpr const char* kPulse       = "Pulse";       ///< 衝撃波
inline constexpr const char* kDash        = "Dash_InPlace";///< 低空の突進
inline constexpr const char* kTurnL       = "Turn_L_InPlace";
inline constexpr const char* kTurnR       = "Turn_R_InPlace";
inline constexpr const char* kShowcase    = "Showcase";    ///< お披露目。遊びでは使わない

/// 自分の意思で抜けられる (＝手を割り込ませてよい) ステート。
inline constexpr const char* kIdle         = "Idle";
inline constexpr const char* kHoverForward = "Hover_Forward";
inline constexpr const char* kHoverLeft    = "Hover_Left";
inline constexpr const char* kHoverRight   = "Hover_Right";

/// 加算の被弾レイヤー。IsClosed / IsStaggered / IsDead のいずれかが立つと鳴らない。
inline constexpr const char* kHitLayer = "Hit";
} // namespace boss03anim

/// モデルの «正面» が +Z からどれだけ回っているか [度]。
///
/// @note `Boss03.fbx` の性質で調整値ではない。AI の向き直りと Animator の前/左/右判定の両方が参照するため定数へ集約する。
/// @note 正面はローカル -Z。両判定で同じ 180 度補正を使う。
inline constexpr float kBoss03FacingOffsetDegrees = 180.0f;

/// 浮遊の向き。Motion は float だが値は離散で、遷移条件は Equal で引いている。
///
/// @note 実速でなく離散値を渡す。Hover は `Motion == 1/2/3` の Equal 条件で遷移するため、連続値だとどれにも一致せず Idle のまま浮く。
enum class Boss03Motion : int {
    Hold    = 0,
    Forward = 1,
    Left    = 2,
    Right   = 3,
};

/// もぎ取れる翼。進行の物差しであり、連撃の長さの上限でもある。
enum class Boss03Wing : int {
    LeftUpper   = 0,
    LeftMiddle  = 1,
    LeftLower   = 2,
    RightUpper  = 3,
    RightMiddle = 4,
    RightLower  = 5,
};

inline constexpr int kBoss03WingCount = 6;

/// 翼を落とすトリガー名。**Detach レイヤーの名前も同じ綴り**で、
/// Boss03.animcontroller はそのレイヤーの Detached ステートで畳んだ姿を保持する。
inline constexpr const char* kBoss03WingTriggers[kBoss03WingCount] = {
    "Detach_L_Upper", "Detach_L_Middle", "Detach_L_Lower",
    "Detach_R_Upper", "Detach_R_Middle", "Detach_R_Lower",
};

/// 翼の骨名 (Boss03.fbx)。当たり判定のノードから «どの翼か» を引くのに使う。
inline constexpr const char* kBoss03WingBones[kBoss03WingCount] = {
    "Wing_L_Upper", "Wing_L_Middle", "Wing_L_Lower",
    "Wing_R_Upper", "Wing_R_Middle", "Wing_R_Lower",
};

/// 投げた翼を «静的に» 描くために生やす子の名前の頭。
/// 戻すときはこれで見分けて畳むので、翼の子に同じ頭の名前を置かないこと。
inline constexpr const char* kBoss03WingPieceName = "FX_WingPiece_";

/// 名前から翼を引く。見つからなければ -1。
///
/// @note 完全一致でなく部分一致。当て先は骨自体でなく骨へ提げた当たり判定 (`HB_Wing_L_Upper` 等) の名前になるため。
[[nodiscard]] inline int Boss03WingFromName(std::string_view name)
{
    for (int i = 0; i < kBoss03WingCount; ++i)
        if (name.find(kBoss03WingBones[i]) != std::string_view::npos) return i;
    return -1;
}

} // namespace sandbox
