// FBZZ Engine
// PlayerTuning.hpp | sandbox
// プレイヤーの移動・ジャンプ・回避・体力をまとめた共有データアセット (.fzdata)。
//
// WHY 極性側と分けるか:
//   PolarityTuning は敵・柱・銃が共通で参照する「盤面のルール」で、触ると全体の
//   成立条件 (7.7) が動く。こちらはプレイヤーの手触りだけに閉じた値で、いくら振っても
//   ゲームの成立には影響しない。触ったときの波及範囲が違うものを 1 枚にすると、
//   手触りを直すたびにルールの数値まで目に入って判断が鈍る。
#pragma once

#include <Engine/Asset/DataAsset.hpp>

namespace sandbox {

class PlayerTuning : public fbzz::DataAsset {
    FBZZ_DATA_ASSET(PlayerTuning)
public:
    FBZZ_GROUP("Move")
    FBZZ_FIELD_RANGE(float, moveSpeed,   6.0f,  "Move Speed",   0.1f, 20.0f)
    FBZZ_FIELD_RANGE(float, groundAccel, 18.0f, "Ground Accel", 1.0f, 100.0f)
    FBZZ_FIELD_RANGE(float, groundDecel, 22.0f, "Ground Decel", 1.0f, 100.0f)
    FBZZ_FIELD_RANGE(float, turnSpeed,   14.0f, "Turn Speed",   0.1f, 30.0f)
    // CharacterController::JumpAtVelocity へ渡す上向き初速 (m/s)。
    // 質量に依存しないため、RigidBody の密度を変えても跳躍感を維持できる。
    FBZZ_FIELD_RANGE(float, jumpSpeed,   7.0f,  "Jump Speed",   0.1f, 30.0f)

    FBZZ_GROUP("Dodge")
    // 回避は移動アクションとしてのみ実装する。ジャスト回避の判定・無敵・報酬は
    // 19 章で意図的に外しているため、ここに無敵時間のフィールドは置かない。
    // 後から足す場合はこのアセットに 1 行加えるだけで済む。
    FBZZ_FIELD_RANGE(float, dodgeSpeed,    16.0f, "Dodge Speed",    1.0f, 60.0f)
    FBZZ_FIELD_RANGE(float, dodgeDuration, 0.22f, "Dodge Duration", 0.05f, 1.0f)
    FBZZ_TOOLTIP("この秒数だけ水平速度を回避方向で上書きする")
    FBZZ_FIELD_RANGE(float, dodgeCooldown, 0.8f, "Dodge Cooldown", 0.0f, 5.0f)

    FBZZ_GROUP("Health")
    // 18.3 で「HP 制を採用する (数発耐える)」まで確定済み。具体値はここで詰める。
    // WHY _INT 版か: FBZZ_FIELD_RANGE は IReflector::FloatRange へ流すため float 専用。
    //      int を渡すと float& へバインドできずコンパイルエラーになる。
    FBZZ_FIELD_RANGE_INT(int, maxHealth, 5, "Max Health", 1, 50)
    // 被弾直後の無敵。連続ヒットで一瞬に溶けるのを防ぐためのもので、
    // 回避の無敵 (未実装) とは別物。
    FBZZ_FIELD_RANGE(float, hitInvulnerable, 0.6f, "Hit Invulnerable", 0.0f, 3.0f)
};

FBZZ_REFLECT(PlayerTuning)

} // namespace sandbox
