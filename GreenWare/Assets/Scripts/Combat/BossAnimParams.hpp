/// @file BossAnimParams.hpp
/// @brief Boss.animcontroller のパラメーター名とステート名の一元定義
/// @author Hasegawa Jin
/// @date 2026-08-26
///
/// WHY 定数にするか:
///   SetFloat / SetTrigger は名前が違っても黙って何もしない。綴りを間違えた症状が
///   「アニメーションだけ動かない」で、コードを読んでも原因が出ない。.animcontroller
///   側の綴りと突き合わせる場所を 1 箇所へ閉じる。WeaponSockets.hpp と同じ理由。
///
/// WHY BossAnimatorComponent.hpp に同居させないか:
///   ScriptCodeGen はスクリプト登録マクロの直前に現れた «最後の namespace 行» を
///   そのまま登録名前空間として採る (閉じ括弧を追わない)。スクリプト本体の手前に
///   入れ子の名前空間を書くと存在しない名前空間で登録され、ScriptList.inl が
///   コンパイルできなくなる。定数側をファイルごと外へ出す。
///
/// NOTE: 同じ理由で、この階層のヘッダーのコメントに登録マクロを «そのままの綴りで»
///       書いてはいけない。ScriptCodeGen はコメントを読み飛ばさないため、説明のつもりの
///       1 行がクラス名として登録される。
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
