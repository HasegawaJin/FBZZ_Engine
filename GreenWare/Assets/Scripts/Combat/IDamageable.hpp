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
///   使う側は名簿から引く (下の Of()。理由はそこに書いてある):
///     if (auto* target = IDamageable::Of(hitObject))
///         target->ApplyDamage(10);
#pragma once

#include <Math/Vector3.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/PlayerHit.hpp>
#include <unordered_map>

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

    /// 一撃を受ける。無敵・回避・弾きで防げたかまで含めて返す。
    /// @param fromWorld 当たった物の位置。弾きの絵と押しの向きに使う。nullptr 可。
    /// @param kind      刀で弾ける一撃か。
    ///
    /// WHY ApplyDamage と別に持つか: 弾けるのはプレイヤーだけで、敵や的は «通ったか» しか
    ///     持たない。既定は ApplyDamage へ流すだけにして、弾ける側だけが上書きする。
    virtual PlayerHitResult ReceiveHit(int amount, const fbzz::math::Vector3* fromWorld,
                                       PlayerHitKind kind)
    {
        (void)fromWorld; (void)kind;
        return ApplyDamage(amount) ? PlayerHitResult::Damaged : PlayerHitResult::Ignored;
    }

    [[nodiscard]] virtual int  CurrentHealth() const = 0;
    [[nodiscard]] virtual int  MaxHealth()     const = 0;
    [[nodiscard]] virtual bool IsAlive()       const = 0;

    /// 殴られた «向き» を受け取る。押されない相手は何もしない。
    /// @param fromWorld 当たった物の位置。ここから離れる向きへ押す。
    ///
    /// WHY ダメージと同じ入口に置くか: 押す / 押さないは «殴られた» という 1 つの
    ///     出来事の一部で、敵ごとに書くと «この敵だけ押されない» が普通に起きる。
    ///     押し方を知っているのは殴られた側なので、宣言だけをここへ置く。
    ///
    /// WHY 純粋仮想にしないか: 押されない相手 (据え付けの的・ボス) の方が多く、
    ///     そちらに «空の実装» を書かせる理由が無い。
    virtual void ApplyKnockback(const fbzz::math::Vector3& fromWorld, float speed, float seconds)
    {
        (void)fromWorld; (void)speed; (void)seconds;
    }

    // ── 引き当て ────────────────────────────────────────────────────────────
    // WHY scene.GetScript<IDamageable>() を使わないか:
    //   あれは Script::FbzzAsType の基底たどりに乗っているが、この環境では
    //   «自分の具象型» にしか答えが返らない。実測 (2026-08-30):
    //       IsA("EnemyHealthComponent") = 1   ← 自分の型
    //       IsA("Script")               = 0   ← 基底。本来 1 のはず
    //       IsA("IDamageable")          = 0   ← インターフェース
    //   TryBases<Script, IDamageable>(...) を直に叩いても同じなので、
    //   マクロ展開ではなくテンプレート側の問題。エンジンが直るまでは、
    //   «殴られる側» が自分で名乗る名簿で引く。
    //
    // WHY GameObject* をキーにするか:
    //   EntityID はハッシュを持たない。ポインタは «消えた後に別物へ再利用される»
    //   のが怖いが、OnDestroy で必ず降ろすので、生きている間しか載っていない。
    static std::unordered_map<const GameObject*, IDamageable*>& Registry()
    {
        static std::unordered_map<const GameObject*, IDamageable*> map;
        return map;
    }

    /// 実装側が OnStart で名乗る。
    static void Bind(const GameObject* go, IDamageable* self)
    {
        if (go && self) Registry()[go] = self;
    }
    /// 実装側が OnDestroy で降りる。自分が載っているときだけ消す
    /// (プールで同じ GameObject を使い回したとき、後から来た方を消さないため)。
    static void Unbind(const GameObject* go, const IDamageable* self)
    {
        if (!go) return;
        auto it = Registry().find(go);
        if (it != Registry().end() && it->second == self) Registry().erase(it);
    }
    /// 名簿から引く。載っていなければ nullptr。
    [[nodiscard]] static IDamageable* Of(const GameObject* go)
    {
        if (!go) return nullptr;
        auto it = Registry().find(go);
        return it == Registry().end() ? nullptr : it->second;
    }
};

} // namespace sandbox
