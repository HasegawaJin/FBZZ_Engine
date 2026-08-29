/// @file    IDamageable.hpp
/// @brief   ダメージを受け取れるものが実装する横断インターフェース
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// WHY 基底クラスではなくインターフェースか:
///   ダメージを受けるものは敵だけではない。プレイヤーも、将来置く破壊可能な障害物も
///   受ける。一方でこの 3 者に共通の「実装」は無く、HP の持ち方も無敵時間の有無も違う。
///   共有したいのは能力の宣言だけなので、状態を持たないインターフェースが正しい形になる。
///
/// WHY Script を継承しないか:
///   Script を継承したクラスを 2 つ以上継承すると Script 部分オブジェクトが 2 個になり、
///   ScriptComponent が持つ unique_ptr<Script> が曖昧になる (Script.hpp の
///   FBZZ_SCRIPT_DERIVED の注記)。能力の側が状態を持たなければ、その制約は要らない。
///
///   使う側は型で引ける:
///     if (auto* target = scene.GetScript<IDamageable>(hitObject))
///         target->ApplyDamage(10);
///     for (GameObject* go : scene.FindObjectsOfType<IDamageable>()) ...
#pragma once

#include <Engine/Scene/Script.hpp>

using namespace fbzz::scene;

namespace sandbox {

struct IDamageable {
    FBZZ_SCRIPT_INTERFACE(IDamageable)

    /// 固定量のダメージを適用する。
    /// @param amount 1 以上。0 以下は何もせず false。
    /// @ret 実際に HP が減ったら true。無敵時間・死亡済みで弾かれたら false。
    ///
    /// WHY 「死んだか」ではなく「減ったか」を返すか: 死亡の検知は CurrentHealth() /
    ///     IsAlive() を前後で見れば足りるが、「入ったのか弾かれたのか」は呼び出し側から
    ///     観測できない。無敵時間で弾かれた分を戦果に数えないために、こちらを返す。
    virtual bool ApplyDamage(int amount) = 0;

    [[nodiscard]] virtual int  CurrentHealth() const = 0;
    [[nodiscard]] virtual int  MaxHealth()     const = 0;
    [[nodiscard]] virtual bool IsAlive()       const = 0;
};

} // namespace sandbox
