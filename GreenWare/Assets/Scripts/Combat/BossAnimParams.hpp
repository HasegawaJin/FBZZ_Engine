/// @file    BossAnimParams.hpp
/// @brief   Boss.animcontroller のパラメーター名とステート名の一元定義
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// @note `SetFloat`/`SetTrigger` は綴りが違っても黙って無視するため、`.animcontroller` 側との突き合わせ箇所を 1 箇所に閉じる。`WeaponSockets.hpp` と同じ理由。
/// @note ScriptCodeGen はスクリプト登録マクロ直前の最後の namespace 行をそのまま登録名に採る。`BossAnimatorComponent.hpp` に同居させ入れ子 namespace を書くと存在しない名前空間で登録され `ScriptList.inl` が壊れる。
/// @note 同じ理由でこの階層のヘッダーのコメントに登録マクロをそのままの綴りで書かない。ScriptCodeGen はコメントも読むため、説明のつもりの 1 行がクラス名として登録される。
#pragma once

namespace sandbox {

/// Boss.animcontroller のパラメーター名。
namespace bossanim {
inline constexpr const char* kSpeed    = "Speed";    ///< 水平速度 (m/s)。Idle ⇄ Walk_Crawl
inline constexpr const char* kTurn     = "Turn";     ///< 旋回 -1..+1 (+ が左)。その場旋回の選択
inline constexpr const char* kStomp    = "Stomp";    ///< 単脚踏みつけの発火
inline constexpr const char* kStompLeg = "StompLeg"; ///< 踏みつける脚 (BossLeg の値)
inline constexpr const char* kJump     = "Jump";     ///< 大ジャンプの踏み切り
inline constexpr const char* kGrounded = "Grounded"; ///< 接地。false の間 FallIdle を回す
inline constexpr const char* kPulse    = "Pulse";    ///< 磁力パルス
inline constexpr const char* kCharge   = "Charge";   ///< 突進の溜め開始
inline constexpr const char* kCharging = "Charging"; ///< 突進中。false で Locomotion へ抜ける
inline constexpr const char* kCrash    = "Crash";    ///< 突進が激突して転倒
inline constexpr const char* kBeam     = "Beam";     ///< ビームの構え開始
inline constexpr const char* kBeaming  = "Beaming";  ///< 照射中。false で Beam_End へ
inline constexpr const char* kHit      = "Hit";      ///< 加算レイヤーの被弾リアクション
inline constexpr const char* kIsDead   = "IsDead";   ///< 死亡ステートへの分岐
/// ステートマシンが自分の意思で抜けられる (＝行動を割り込ませてよい) ステート。
inline constexpr const char* kLocomotion = "Locomotion";
inline constexpr const char* kTurnLeft   = "TurnLeft";
inline constexpr const char* kTurnRight  = "TurnRight";
} // namespace bossanim

/// 踏みつける脚。値は .animcontroller の StompLeg 条件と 1 対 1 で対応する。
enum class BossLeg : int {
    FrontRight = 0,
    FrontLeft  = 1,
    BackRight  = 2,
    BackLeft   = 3,
};

} // namespace sandbox
