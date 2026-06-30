// FBZZ Engine
// EnemyStats.hpp | sandbox
// 敵の調整値をまとめた共有データアセット (Unity の ScriptableObject 相当)。
//
// 設計意図 (WHY):
//   敵が複数体いても「強さ」を 1 つの .fzdata で一元管理したい。各 EnemyController が
//   Asset<EnemyStats> で同じ実体を参照すれば、1 か所いじるだけで全個体のバランスが変わる。
//   値をコンポーネントのインスタンスフィールドに持たせると個体ごとに複製され、リバランスが
//   各個編集になってしまう問題を解消する。
//
//   フィールドは FBZZ_FIELD 系をそのまま使える (スクリプトと同じリフレクション基盤)。
//   .fzdata を Asset ブラウザで選べば Inspector でこれらを編集でき、編集は即座に全参照へ反映される。
#pragma once

#include <Engine/Asset/DataAsset.hpp>
#include <string>

namespace sandbox {

class EnemyStats : public fbzz::DataAsset {
    FBZZ_DATA_ASSET(EnemyStats)

public:
    FBZZ_GROUP("Movement")
    FBZZ_FIELD_RANGE(float, moveSpeed,   2.8f,  "Move Speed",   0.1f, 12.0f)
    FBZZ_TOOLTIP("追跡時の水平移動速度 (m/s)")
    FBZZ_FIELD_RANGE(float, stopDistance, 1.15f, "Stop Distance", 0.1f, 5.0f)
    FBZZ_TOOLTIP("ターゲットへこの距離まで近づいたら停止する")

    FBZZ_GROUP("Attack")
    FBZZ_FIELD_RANGE(float, attackRange,    1.35f, "Attack Range",    0.1f,  5.0f)
    FBZZ_TOOLTIP("この距離以内に入ると攻撃を開始する")
    FBZZ_FIELD_RANGE(float, attackCooldown, 1.20f, "Attack Cooldown", 0.1f, 10.0f)
    FBZZ_TOOLTIP("次の攻撃までの待機秒数")
    FBZZ_FIELD_RANGE(float, knockbackSpeed, 2.50f, "Knockback Speed", 0.0f, 12.0f)

    FBZZ_GROUP("Vitality")
    FBZZ_FIELD_RANGE(float, maxHp, 100.0f, "Max HP", 1.0f, 9999.0f)

    FBZZ_GROUP("Display")
    // 例: バランス表でこの敵タイプを識別するための表示名。
    FBZZ_FIELD(std::string, displayName, "Grunt", "Display Name")
};

// Reflect() をフィールド宣言から自動生成する。
FBZZ_REFLECT(EnemyStats)

} // namespace sandbox
