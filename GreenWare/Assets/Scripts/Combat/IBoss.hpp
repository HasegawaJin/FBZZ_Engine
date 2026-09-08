/// @file    IBoss.hpp
/// @brief   ボスが実装する横断インターフェース。「雑魚と違って何を持っているか」だけを宣言する
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY 基底クラスではなくインターフェースか:
///   ボスは今後増えるが、増えたときに共有したいのは «実装» ではなく «問い合わせ口» の方。
///   HUD のボスバー・BGM・雑魚の狙い先・進行管理は「今のボスが何極で、第何フェーズで、
///   今が攻め時か」を知りたいだけで、それをどう決めているか (周期タイマーなのか HP 閾値
///   なのか、ステートマシンなのか) はボスごとに違う。実装を基底へ引き上げると、
///   2 体目が «違う決め方» をした瞬間に基底へフラグが生えていく。
///   まずは宣言だけを固定し、共有できる実装が実際に 2 体で重複してから
///   BossAiBase を切り出す (IDamageable と同じ «宣言だけ» の判断)。
///
/// WHY Script を継承しないか:
///   Script 派生を 2 つ以上継承すると Script 部分オブジェクトが 2 個になり、
///   ScriptComponent の unique_ptr<Script> が曖昧になる (Script.hpp の注記)。
///   ボスは `Script` + `IDamageable` + `IBoss` を同時に名乗る必要があるので、
///   状態を持たないインターフェース側で表す。
///
/// WHY HP をここに持たないか:
///   ダメージと生死は IDamageable が既に宣言していて、ボスも同じ口で受ける
///   (企画書 7.4 / 10.6「撃破は衝突ダメージのみ」)。ここに CurrentHealth を重ねると、
///   ボスバーがどちらを読むかで割れる。ボスは IDamageable と IBoss を両方名乗ること。
///
///   使う側は名簿 (Of / FindBossOnBoard) から引く。**型では引けない** (下の WHY)。
///
/// WHY 型で引かず名簿を持つか (2026-09-01 の修正):
///   `scene.GetScript<IBoss>()` と `FindObjectsOfType<IBoss>()` は、この環境では
///   横断インターフェースに対して必ず空を返す (基底たどりが効いていない。
///   IDamageable が先に同じ壁に当たっていて、あちらは名簿で回避している)。
///   そのため `StageHasBoss()` が常に false を返し、**ボスを倒しても
///   GameFlowComponent の勝利判定へ一度も入らない** ─ ステージが終わらず
///   リザルトへも移らない、という形で 2 ステージとも壊れていた
///   (Stage_01 のシーンに残っていた `debugBoss = ''` がその跡)。
///   ボスは自分で名乗り、引く側は名簿を見る。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <unordered_map>

using namespace fbzz::scene;

namespace sandbox {

struct IBoss {
    FBZZ_SCRIPT_INTERFACE(IBoss)

    /// ボスバーに出す表示名。
    /// WHY GameObject 名で代用しないか: シーン上の名前は開発の都合で変わるうえ、
    ///     プレイヤーに見せる名前 (「ポラリティ・コア」) とは別物になる。
    [[nodiscard]] virtual const char* BossName() const = 0;

    // ── フェーズ (企画書 10.6) ───────────────────────────────────────────────

    /// 今のフェーズ。1 始まり。
    [[nodiscard]] virtual int CurrentPhase() const = 0;

    /// このボスが持つフェーズ数。
    /// WHY 段数を固定しないか: 10.6 のポラリティ・コアは P1 / P2 の 2 段だが、
    ///     段数はボスごとの設計そのもの。HUD 側が 2 段を前提に描くと 3 段目を
    ///     足した瞬間に表示が壊れる。
    [[nodiscard]] virtual int PhaseCount() const = 0;

    // ── 隙 (企画書 8 章「いずれの攻撃にも明確な隙を設ける」) ──────────────────

    /// 硬直中か。突進の激突スタン・踏みつけ後の脚が刺さっている間などが該当する。
    ///
    /// WHY 「攻撃可能か」ではなく「硬直中か」か: ボスは斬撃では削れず、とどめで部位を
    ///     落とす以外に手が無い。つまりダメージが通るかどうかは常に同じで、
    ///     隙が意味するのは «プレイヤーが安全にコンボを組める時間» の方。
    ///     Vulnerable と名付けると「今なら撃てる」と読まれ、盤面の理解がずれる。
    [[nodiscard]] virtual bool IsStaggered() const = 0;

    // ── 登場 ────────────────────────────────────────────────────────────────

    /// 戦闘が始まっているか。«部屋に入るまで眠っている» ボスは、起きるまで false。
    ///
    /// WHY «居るか» と別に要るか: 眠っているボスも盤面には立っていて、見えている。
    ///     存在だけで «戦闘中» と数えると、部屋へ入る前から体力バーが出て、弾薬の
    ///     供給も始まり、進行は «もう戦っている» と思い込む。
    ///
    /// WHY 純粋仮想にしないか: 登場の条件を持たないボス (置いた瞬間から戦っている)
    ///     の方が普通で、そちらに «true を返すだけ» を書かせる理由が無い。
    [[nodiscard]] virtual bool IsEngaged() const { return true; }

    // ---- 弾いて崩す (Docs/break-parry.md) ----
    // 硬直 (IsStaggered) は踏みつけ後の短い隙も含む。«とどめ» が通るのは崩しゲージが
    // 満ちて倒れている間だけなので、別の問い合わせ口として分ける。

    /// 崩しゲージが満ちて倒れているか。この間だけ Execute() が通る。
    [[nodiscard]] virtual bool IsToppled() const { return false; }
    /// プレイヤーに一撃を弾かれた。hitPoint は弾いた場所。攻撃を出しかけの脚が跳ねる等。
    virtual void OnParried(const fbzz::math::Vector3& hitPoint) { (void)hitPoint; }
    /// 倒れているボスの部位へ «とどめ» が入った。部位を落とせたら true。
    /// @param part 斬った部位 (当たり判定のノード)
    /// @param from 斬った側の位置
    virtual bool Execute(GameObject* part, const fbzz::math::Vector3& from)
    { (void)part; (void)from; return false; }
    /// 進行の物差し。脚なら残り本数、蛇なら残り節数。持たないボスは負を返す。
    [[nodiscard]] virtual int PartsRemaining() const { return -1; }
    [[nodiscard]] virtual int PartsTotal() const { return 0; }

    // ── 名簿 (IDamageable と同じ形) ──────────────────────────────────────────
    // 実装側は OnStart で Bind、OnDestroy で Unbind すること。書き忘れると
    // «倒してもステージが終わらない» という形でしか症状が出ない。

    static std::unordered_map<const GameObject*, IBoss*>& Registry()
    {
        static std::unordered_map<const GameObject*, IBoss*> map;
        return map;
    }

    static void Bind(const GameObject* go, IBoss* self)
    {
        if (go && self) Registry()[go] = self;
    }
    /// 自分が載っているときだけ消す (同じ GameObject を使い回したとき、
    /// 後から来た方を消さないため)。
    static void Unbind(const GameObject* go, const IBoss* self)
    {
        if (!go) return;
        auto it = Registry().find(go);
        if (it != Registry().end() && it->second == self) Registry().erase(it);
    }
    [[nodiscard]] static IBoss* Of(const GameObject* go)
    {
        if (!go) return nullptr;
        auto it = Registry().find(go);
        return it == Registry().end() ? nullptr : it->second;
    }
};

/// 今この瞬間、«戦っている相手» としてのボス。居なければ nullptr。
/// 盤面に出ていて (activeInHierarchy)、かつ交戦が始まっている (IsEngaged) こと。
///
/// WHY 2 つの条件を 1 つの関数にまとめるか:
///   ボスが «まだ相手ではない» 状態は 2 通りある — GameObject ごと畳まれている場合と、
///   立ってはいるが部屋へ入るまで眠っている場合 (BossRoomTriggerComponent)。
///   どちらも進行・体力バー・弾薬の供給にとっては同じ «まだ» なので、区別する理由が無い。
///
/// WHY 存在で数えてはいけないか:
///   まだ動いていないボスはスクリプトが走っていないので HP が 0 に見える。存在だけで
///   «居る» と数えると、その 0 を «撃破済み» と読んで開始と同時にステージがクリアになる。
///
/// WHY 進行・HUD・補充で使い回すか:
///   «ボスと戦っている» は勝利条件・体力バー・弾薬の供給が同時に切り替わる 1 つの節目。
///   判定を各所へ書くと、片方だけ «まだ» だと思っている状態が作れてしまう。
[[nodiscard]] inline GameObject* FindBossOnBoard(const ScriptSceneProxy& scene)
{
    (void)scene;   // 名簿はシーンに依らない。引数は呼ぶ側の書き方を変えないために残す
    for (const auto& [object, boss] : IBoss::Registry()) {
        if (!object || !boss) continue;
        // WHY const を外すか: 名簿のキーは «誰か» を指すためだけの const で、
        //     返す先 (勝利判定・HUD) はその GameObject を普通に読み書きする。
        GameObject* self = const_cast<GameObject*>(object);
        if (!self->activeInHierarchy() || !boss->IsEngaged()) continue;
        return self;
    }
    return nullptr;
}

/// この盤面にボスが «居る» か。眠っていても畳まれていても true。
///
/// WHY 交戦中かどうかと分けるか: «ボス戦の盤面である» ことは戦闘が始まる前から
///     決まっている。進行がこれを見ずに «敵が 0 になったら勝ち» を残すと、
///     部屋へ入る前に盤面が空になった瞬間クリアしてしまう。
[[nodiscard]] inline bool StageHasBoss(const ScriptSceneProxy& scene)
{
    (void)scene;
    for (const auto& [object, boss] : IBoss::Registry())
        if (object && boss) return true;
    return false;
}

} // namespace sandbox
