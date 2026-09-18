/// @file    IDamageable.hpp
/// @brief   ダメージを受け取れるものが実装する横断インターフェース
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// @note 基底クラスではなくインターフェース。HP の持ち方が敵/プレイヤー/破壊物で異なり共通実装が無い。
/// @note Script は継承しない。Script 派生を複数継承すると `unique_ptr<Script>` が曖昧になる
///       (`Script.hpp` の `FBZZ_SCRIPT_DERIVED` 参照)。状態を持たなければこの制約は要らない。
/// @note 使う側は `Of()` で名簿から引く (`if (auto* t = IDamageable::Of(go)) t->ApplyDamage(10);`)。
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
    /// @return 実際に HP が減ったら true。無敵時間・死亡済みで弾かれたら false。
    ///
    /// @note 死亡ではなく «減ったか» を返す。無敵時間で弾かれた分を戦果に数えないため。
    virtual bool ApplyDamage(int amount) = 0;

    /// 一撃を受ける。無敵・回避・弾きで防げたかまで含めて返す。
    /// @param fromWorld 当たった物の位置。弾きの絵と押しの向きに使う。nullptr 可。
    /// @param kind      刀で弾ける一撃か。
    ///
    /// @note 既定は `ApplyDamage` へ流すだけ。弾けるのはプレイヤーだけなので弾ける側だけが上書きする。
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
    /// @note ダメージと同じ入口に置く。押し方を知るのは殴られた側なので、敵ごとに書かせない。
    /// @note 純粋仮想にしない。押されない相手 (据え付けの的・ボス) の方が多く空実装が無駄になる。
    virtual void ApplyKnockback(const fbzz::math::Vector3& fromWorld, float speed, float seconds)
    {
        (void)fromWorld; (void)speed; (void)seconds;
    }

    /// @name 引き当て
    /// @{
    /// @note `scene.GetScript<IDamageable>()` は使わない。基底/インターフェースへの多態解決が壊れており
    ///       自分の具象型にしか一致しない (実測 2026-08-30: `IsA("Script")` `IsA("IDamageable")` とも 0)。
    ///       `TryBases<Script, IDamageable>(...)` を直に叩いても同じでテンプレート側の問題。直るまでは
    ///       «殴られる側» が自分で名乗る名簿で引く。
    /// @note キーは `GameObject*`。`EntityID` はハッシュを持たない。`OnDestroy` で必ず `Unbind()` する。
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
    /// @}
};

} // namespace sandbox
