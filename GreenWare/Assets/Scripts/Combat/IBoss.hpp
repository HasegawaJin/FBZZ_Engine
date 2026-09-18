/// @file    IBoss.hpp
/// @brief   ボスが実装する横断インターフェース。「雑魚と違って何を持っているか」だけを宣言する
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// @note Script を継承しない: 複数の Script 派生を継承すると unique_ptr<Script> が曖昧に
///       なる (Script.hpp 参照)。ボスは `Script` + `IDamageable` + `IBoss` を同時に名乗る。
/// @note HP は持たない: ダメージ・生死は IDamageable の口で受ける。ここに CurrentHealth を
///       重ねるとボスバーの参照元が割れる。
/// @warning `scene.GetScript<IBoss>()` / `FindObjectsOfType<IBoss>()` はこの環境で横断
///       インターフェースに対し常に空を返す。使う側は必ず名簿 (Of / FindBossOnBoard) から
///       引くこと (2026-09-01: 型引きの不具合でボス撃破がステージクリアに繋がらなかった)。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <unordered_map>
#include <vector>

using namespace fbzz::scene;

namespace sandbox {

struct IBoss {
    FBZZ_SCRIPT_INTERFACE(IBoss)

    /// @brief ボスバーに出す表示名。
    /// @note GameObject 名で代用しない: シーン上の名前と表示名 (例: 「ポラリティ・コア」) は別物。
    [[nodiscard]] virtual const char* BossName() const = 0;

    /// @name フェーズ (企画書 10.6)
    /// @{

    /// 今のフェーズ。1 始まり。
    [[nodiscard]] virtual int CurrentPhase() const = 0;

    /// @brief このボスが持つフェーズ数。
    /// @note 固定しない理由: 段数はボスごとの設計そのもの。HUD が段数を決め打ちしないための口。
    [[nodiscard]] virtual int PhaseCount() const = 0;
    /// @}

    /// @name 隙 (企画書 8 章「いずれの攻撃にも明確な隙を設ける」)
    /// @{

    /// @brief 硬直中か。突進の激突スタン・踏みつけ後の脚が刺さっている間など。
    /// @note Vulnerable ではない: ダメージの通り方は常に同じで、意味は「安全にコンボを組める間」。
    [[nodiscard]] virtual bool IsStaggered() const = 0;
    /// @}

    /// @name 登場
    /// @{

    /// @brief 戦闘が始まっているか。「部屋に入るまで眠っている」ボスは起きるまで false。
    /// @note 存在 (居るか) とは別: 存在だけで数えると入室前から HP バー/弾薬供給が始まる。
    /// @note 既定 true: 登場条件を持たないボスの方が普通で、上書きする側だけ実装すればよい。
    [[nodiscard]] virtual bool IsEngaged() const { return true; }
    /// @}

    /// @name 弾いて崩す (Docs/break-parry.md)
    /// @{
    /// 硬直 (IsStaggered) は踏みつけ後の短い隙も含む。«とどめ» が通るのは崩しゲージが
    /// 満ちて倒れている間だけなので、別の問い合わせ口として分ける。

    /// 崩しゲージが満ちて倒れているか。この間だけ Execute() が通る。
    [[nodiscard]] virtual bool IsToppled() const { return false; }
    [[nodiscard]] virtual bool CanExecute() const { return IsToppled(); }
    [[nodiscard]] virtual bool UsesBodyHitbox() const { return false; }
    [[nodiscard]] virtual bool CanTakeBodyDamage() const { return false; }
    virtual bool ApplyBodyDamage(int amount) { (void)amount; return false; }
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

    /// @brief 画面で「その相手が居る」場所。出せなければ false (ルートの位置が使われる)。
    /// @note ルート transform だけでは足りない: 蛇は骨だけが経路上を動き、ルートは据え置き。
    [[nodiscard]] virtual bool FocusPoint(fbzz::math::Vector3& out) const
    { (void)out; return false; }
    /// @}

    /// @name 名簿 (IDamageable と同じ形)
    /// @{
    /// 実装側は OnStart で Bind、OnDestroy で Unbind すること。書き忘れると
    /// «倒してもステージが終わらない» という形でしか症状が出ない。

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
    /// @}
};

/// @brief 今この瞬間「戦っている相手」としてのボス。居なければ nullptr。盤面に出ていて
///        (activeInHierarchy) かつ交戦が始まっている (IsEngaged) こと。
/// @note 存在だけで数えない: 眠っているボスは HP が 0 に見えるため、開始直後にクリア誤判定する。
/// @note 勝利条件・HP バー・弾薬供給はこの 1 関数の結果で揃える。各所で判定すると足並みが崩れる。
[[nodiscard]] inline GameObject* FindBossOnBoard(const ScriptSceneProxy& scene)
{
    /// @note 名簿はシーンに依らない。引数は呼ぶ側の書き方を変えないために残す
    (void)scene;
    for (const auto& [object, boss] : IBoss::Registry()) {
        if (!object || !boss) continue;
        /// @note const_cast の理由: 名簿のキーは識別だけの const で、呼び出し側は書き込みも行う。
        GameObject* self = const_cast<GameObject*>(object);
        if (!self->activeInHierarchy() || !boss->IsEngaged()) continue;
        return self;
    }
    return nullptr;
}

/// @brief この盤面にボスが「居る」か。眠っていても畳まれていても true。
/// @note 交戦中判定 (FindBossOnBoard) とは別に持つ: これを見ずに「敵0で勝ち」を残すと、
///       部屋へ入る前に盤面が空になった瞬間クリアしてしまう。
[[nodiscard]] inline bool StageHasBoss(const ScriptSceneProxy& scene)
{
    (void)scene;
    for (const auto& [object, boss] : IBoss::Registry())
        if (object && boss) return true;
    return false;
}

/// @brief 盤面に立っているボスを全部。眠っている相手も含む。
/// @note 1 体だけ返す口とは別にする: 名簿は順序を持たないため、複数ボスの盤面で
///       「どれか1体」を使うと毎フレーム別の相手を見て判定が食い違う。
inline void CollectBossesInStage(const ScriptSceneProxy& scene, std::vector<GameObject*>& out)
{
    (void)scene;
    out.clear();
    for (const auto& [object, boss] : IBoss::Registry()) {
        if (!object || !boss) continue;
        GameObject* self = const_cast<GameObject*>(object);
        if (!self->activeInHierarchy()) continue;
        out.push_back(self);
    }
}

/// 盤面に出ていて、かつ交戦が始まっているボスを全部 (FindBossOnBoard の複数版)。
inline void CollectBossesOnBoard(const ScriptSceneProxy& scene, std::vector<GameObject*>& out)
{
    CollectBossesInStage(scene, out);
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const GameObject* object) {
                                 const IBoss* boss = IBoss::Of(object);
                                 return !boss || !boss->IsEngaged();
                             }),
              out.end());
}

/// 戦っている相手のうち point に一番近い 1 体。居なければ nullptr。
/// 近さは FocusPoint (蛇なら頭) で測る ─ ルートは据え置きのことがある。
[[nodiscard]] inline GameObject* FindNearestBossOnBoard(const ScriptSceneProxy& scene,
                                                        const fbzz::math::Vector3& point)
{
    GameObject* best     = nullptr;
    float       bestDist = 0.0f;
    for (const auto& [object, boss] : IBoss::Registry()) {
        if (!object || !boss) continue;
        GameObject* self = const_cast<GameObject*>(object);
        if (!self->activeInHierarchy() || !boss->IsEngaged()) continue;

        fbzz::math::Vector3 at = self->transform.worldPosition;
        (void)boss->FocusPoint(at);
        const float distance = (at - point).LengthSq();
        if (best && distance >= bestDist) continue;
        best     = self;
        bestDist = distance;
    }
    return best;
}

} // namespace sandbox
