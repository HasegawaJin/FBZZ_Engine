/// @file    PlayerTuning.hpp
/// @brief   プレイヤーの移動・ジャンプ・回避・体力をまとめた共有データアセット (.fzdata)。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// WHY 極性側と分けるか:
/// PolarityTuning は敵・柱・銃が共通で参照する「盤面のルール」で、触ると全体の
/// 成立条件 (7.7) が動く。こちらはプレイヤーの手触りだけに閉じた値で、いくら振っても
/// ゲームの成立には影響しない。触ったときの波及範囲が違うものを 1 枚にすると、
/// 手触りを直すたびにルールの数値まで目に入って判断が鈍る。
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
    FBZZ_FIELD_RANGE(float, airAccel,     3.0f, "Air Accel",     0.0f,  50.0f)
    FBZZ_FIELD_RANGE(float, turnSpeed,   14.0f, "Turn Speed",   0.1f, 30.0f)

    FBZZ_GROUP("Jump")
    // WHY 初速ではなく到達点で持つか:
    //   重力は ProjectSettings の [physics] gravity が正本で、世界の全剛体が共有する。
    //   跳躍を初速で持つと、重力を触るたびに跳べる高さが黙って変わり、
    //   「どこまで跳べるか」というレベルデザインの前提が数値の外で崩れる。
    //   到達点で持てば初速は重力から導かれ (v0 = sqrt(2gh))、重力を強くしても
    //   高さは保たれたまま滞空だけが縮む。重力は手触りの、到達点は設計の調整値になる。
    //   到達までの秒数は t = v0/g で決まる結果なので、ここには置かない。
    FBZZ_FIELD_RANGE(float, jumpApexHeight, 2.8f, "Apex Height", 0.2f, 8.0f)
    FBZZ_TOOLTIP("キーを押し続けたときの最高到達点 (m)。キャラの背丈と比べて決める。"
                 "滞空の長さは ProjectSettings の重力で決まる。"
                 "ボスの衝撃波を跳んで越える必要があるので、Clear Height より十分高く保つこと")
    // WHY 落ちを速くするか: 上りと下りが同じ加速度だと、放物線の頂点が長く感じられて
    //     「浮いている」と読まれる。落ちだけ強くすると、頂点が尖って重さが出る。
    //     これは世界の重力に対する倍率なので、重力を触っても比は保たれる。
    FBZZ_FIELD_RANGE(float, fallGravityMultiplier, 1.8f, "Fall Gravity x", 1.0f, 5.0f)
    FBZZ_TOOLTIP("下降中の重力倍率。上げるほど頂点で粘らず落ちる")
    // WHY 離したら重くするか: 押しっぱなしと軽く叩くで高さが変わらないと、跳躍が
    //     1 種類の動作になる。段差を越えるだけの小さい跳びが操作で作れるようにする。
    FBZZ_FIELD_RANGE(float, lowJumpGravityMultiplier, 2.6f, "Low Jump Gravity x", 1.0f, 8.0f)
    FBZZ_TOOLTIP("上昇中にジャンプキーを離している間の重力倍率。1 で高さ固定の跳躍になる")

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
