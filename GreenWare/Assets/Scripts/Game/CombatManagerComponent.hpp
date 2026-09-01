/// @file    CombatManagerComponent.hpp
/// @brief   ダメージの適用を 1 箇所に集め、戦果を数える
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY ダメージを「与える側」ではなくここで適用するか:
///   以前は極性衝突だけがここを通り、敵からプレイヤーへの接触ダメージは
///   敵の接触攻撃が PlayerComponent::TakeDamage() を直接呼んでいた。
///   ダメージの経路が 2 本あると、「無敵時間を入れたい」「与ダメージを記録したい」
///   といった 1 つの要求が必ず 2 箇所の編集になり、片方を忘れた側だけ仕様から外れる。
///   実際、撃破数は数えられているのに被ダメージはどこにも残っていなかった。
///   誰が誰へ何点入れるかは、この 1 本の API を通す。
///
/// WHY ダメージ「値」までは持たないか:
///   敵ごとの耐久や攻撃力は敵の種類の性質で、EnemyHealthComponent /
///   攻撃する側に載っているのが正しい。ここが持つのは「適用と集計」で、
///   値まで吸い上げると敵を 1 種類足すたびにこのファイルが伸びる。
///
/// WHY GameFlowComponent から切り出すか:
///   戦闘の解決 (誰が何ダメージ受けたか) とゲーム進行の管理 (勝敗・HUD・シーン遷移) は、
///   触る理由も触る頻度も違う。ダメージ式は 18.2 が未決なので何度も触るが、リザルトへの
///   遷移条件はほとんど変わらない。同居していると、ダメージを 1 行変えるたびに
///   シーン遷移のコードを読む羽目になり、逆に遷移条件を直すと戦闘の解決まで巻き込む。
///
/// WHY 衝突の購読口をここ 1 箇所にするか:
///   PolarityFieldComponent::onImpact は単一のコールバックで、後から代入した側が勝つ。
///   複数のスクリプトが購読しにいくと「どちらが生きているか」が登録順に依存し、
///   ダメージが入ったり入らなかったりする。盤面からの衝突を受けるのはこのスクリプトだけ、
///   と決めて、戦果は取得用の API で他へ渡す。
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/CharacterEvent.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Combat/EyeSpriteComponent.hpp>
#include <Scripts/Combat/IDamageable.hpp>
#include <Scripts/Polarity/KillCoreComponent.hpp>
#include <Scripts/Polarity/PolarityFieldComponent.hpp>
#include <Scripts/Utils/ManagerWatch.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace fbzz::scene;

namespace sandbox {

class CombatManagerComponent : public Script {
    FBZZ_SCRIPT(CombatManagerComponent)

public:
    // WHY チェインの長さをここが持つか: 「1 回の仕掛けがどれだけ連鎖したか」は
    //     衝突を数えることそのもので、既にこのスクリプトが数えている戦果の一種。
    //     表示側に持たせると、HUD を消しただけで数え方まで消える。
    FBZZ_GROUP("Chain")
    FBZZ_FIELD_RANGE(float, chainSeconds, 2.2f, "Chain Window", 0.2f, 10.0f)
    FBZZ_TOOLTIP("次の衝突がこの秒数以内なら同じチェインとして数える")
    // WHY 連鎖の «深さ» をダメージに乗せるか:
    //   1 発ずつの衝突は等価で、10 連鎖しても «同じ一撃が 10 回» にしかならない。
    //   仕込みの長さが結果に出ないと、丁寧に並べる理由が «倒せる数» だけになり、
    //   1 体ずつ処理するのと変わらなくなる。後ろの一撃ほど重くすれば、
    //   «どこで終わらせるか» が組み立ての目的になる (最後をボスへ当てる、など)。
    //
    // WHY 上限を置くか:
    //   倍率が青天井だと、盤面に敵が多い局面ほど 1 手で全部が消し飛ぶ。
    //   長く繋ぐ意味は残しつつ、伸ばしきったところで頭打ちにする。
    FBZZ_FIELD_RANGE(float, chainDamageStep, 0.35f, "Damage / Link", 0.0f, 2.0f)
    FBZZ_TOOLTIP("連鎖 1 つごとに基礎ダメージへ加算する割合。0 で従来どおり «どの一撃も等価»")
    FBZZ_FIELD_RANGE(float, chainDamageMax, 4.0f, "Damage Cap x", 1.0f, 12.0f)
    FBZZ_TOOLTIP("倍率の上限。1.0 で倍率そのものを切る")
    FBZZ_FIELD_RANGE_INT(int, highChainCount, 4, "High Chain At", 2, 20)
    FBZZ_TOOLTIP("この長さに達したチェインは «伸びた» 側の音に変わる。"
                 "2 に下げると連鎖のたびに鳴るので、めったに出ない長さを置く")

    // 倒した敵がその場に残す一時アンカー (Docs/polarity-system.md「撃破コア」)。
    //
    // WHY 生成をここが持つか: «倒れた» を最初に知るのはこの 1 箇所だけで、
    //     他へ渡すと «倒れたかどうか» の判定が 2 つになる。撃破数の集計と
    //     コアの生成は同じ 1 つの出来事から出ている。
    FBZZ_GROUP("Kill Core")
    FBZZ_FIELD(bool, spawnKillCores, true, "Spawn Kill Cores")
    FBZZ_TOOLTIP("倒した敵の位置に、逆極の的として使える一時アンカーを残す。"
                 "切ると多対 1 の集束は Roller を的にしたときしか成立しなくなる")
    FBZZ_FIELD_RANGE_INT(int, maxKillCores, 6, "Max Cores", 1, 32)
    FBZZ_TOOLTIP("同時に置ける数。多いと盤面が的だらけになり «敵を組む» 必要が消える")
    FBZZ_FIELD_RANGE(float, killCoreHeight, 0.9f, "Height", 0.0f, 5.0f)
    FBZZ_TOOLTIP("床からの高さ [m]。0 だと的が地面に埋まり、飛んできた敵が足元へ潜り込む")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugKills, 0, "Kills")
    FBZZ_FIELD_READ_ONLY(int, debugPushKills, 0, "Push Kills")
    FBZZ_TOOLTIP("反発で押し込んで落とした数。0 のままなら押しが道具として成立していない "
                 "(Docs/development-plan.md の物差し)")
    FBZZ_FIELD_READ_ONLY(int, debugCoresAlive, 0, "Cores Alive")
    FBZZ_FIELD_READ_ONLY(int, debugEnemyImpacts, 0, "Enemy Impacts")
    FBZZ_FIELD_READ_ONLY(int, debugAnchorImpacts, 0, "Anchor Impacts")
    FBZZ_FIELD_READ_ONLY(int, debugDamageToEnemies, 0, "Damage Dealt")
    FBZZ_FIELD_READ_ONLY(int, debugDamageToPlayer, 0, "Damage Taken")
    FBZZ_FIELD_READ_ONLY(int, debugChain, 0, "Chain")
    FBZZ_FIELD_READ_ONLY(int, debugChainHits, 0, "Chain Hits")
    FBZZ_FIELD_READ_ONLY(int, debugBestChain, 0, "Best Chain")
    FBZZ_FIELD_READ_ONLY(int, debugFaces, 0, "Faces")
    FBZZ_TOOLTIP("出来事を受け取れる目の数。想定より少なければ、そのキャラクターに "
                 "EyeSpriteComponent が付いていない")

    /// 誰から呼ばれるかは決まっていない (敵 AI・罠・盤面)。置き場所に依存しない窓口を持つ。
    [[nodiscard]] static CombatManagerComponent* Instance() { return s_instance; }

    [[nodiscard]] int Kills() const { return m_kills; }
    [[nodiscard]] int EnemyImpacts() const { return m_enemyImpacts; }
    [[nodiscard]] int AnchorImpacts() const { return m_anchorImpacts; }
    [[nodiscard]] int DamageDealt() const { return m_damageToEnemies; }
    [[nodiscard]] int DamageTaken() const { return m_damageToPlayer; }
    /// 反発で壁やハザードへ押し込んで落とした数。ランク評価の «押し» の軸。
    [[nodiscard]] int PushKills() const { return m_pushKills; }
    /// 床ハザードのように、盤面の衝突を経由せず «押し込みで倒した» 経路が申告する。
    void AddPushKill() { ++m_pushKills; debugPushKills = m_pushKills; }

    /// 続いている連鎖の長さ。途切れると 0 に戻る。
    [[nodiscard]] int ChainCount() const { return m_chain; }
    /// このプレイでの最長。リザルトで使う。
    [[nodiscard]] int BestChain() const { return m_bestChain; }

    /// この連鎖でここまでに «当たった数»。深さ (m_chain) とは別に数える。
    [[nodiscard]] int ChainHits() const { return m_chainHits; }

    /// 倍率。連鎖の «深さ» ではなく «当たった数» で伸ばす。
    ///
    /// WHY 深さで数えないか:
    ///   深さは 1 フレーム 1 しか進まない (順番に繋いだことを表す値なので正しい)。
    ///   ところが集束は 4 体が同じフレームに着弾するため、深さは +1 にしかならず、
    ///   «仕込んで一斉に叩き込む» という一番大きい手が倍率をまったく受け取れない。
    ///   受け止めた «回数» で数えれば、順番に繋いでも一斉に落としても同じだけ伸びる。
    ///
    /// WHY 1 発目を 1.0 に据えるか: ResolveImpact は数えてから殴るので、最初の一撃で
    ///     既に 1。そのまま掛けると «仕掛けただけ» の一撃が増えてしまう。
    [[nodiscard]] float ChainDamageMultiplier() const
    {
        const int links = m_chainHits > 1 ? m_chainHits - 1 : 0;
        const float raw = 1.0f + std::max(chainDamageStep, 0.0f) * static_cast<float>(links);
        return std::min(raw, std::max(chainDamageMax, 1.0f));
    }
    /// 連鎖が途切れるまでの残り (1 → 直後 / 0 → 途切れた)。表示の減衰に使う。
    [[nodiscard]] float ChainRemaining01() const
    {
        return chainSeconds <= 0.0f ? 0.0f
                                    : std::clamp(m_chainRemaining / chainSeconds, 0.0f, 1.0f);
    }

    /// 敵を 1 体倒した瞬間に呼ばれる。撃破の演出を挿す口。
    std::function<void()> onEnemyDefeated;

    /// プレイヤーへのダメージ。敵の接触攻撃など、盤面を経由しない経路はここを通す。
    /// 実際に減ったら true。無敵時間などで弾かれた場合は false。
    ///
    /// @param source 押し出しの起点 (当たった物の位置)。渡すと «そこから離れる» 向きへ
    ///               押す。nullptr なら押さない。
    ///
    /// WHY 押しをここで配るか: 押す / 押さないは «殴られた» という 1 つの出来事の
    ///     一部で、敵ごとに書くと «この敵だけ押されない» が普通に起きる。
    ///     揺れと振動を ImpactFeedbackManager が 1 箇所で配っているのと同じ形。
    bool DamagePlayer(GameObject* player, int amount, const Vector3* source = nullptr);

    FBZZ_GROUP("Knockback")
    FBZZ_FIELD_RANGE(float, playerKnockSpeed, 7.0f, "Speed", 0.0f, 30.0f)
    FBZZ_TOOLTIP("被弾で押し出される初速 [m/s]。0 で押さない")
    FBZZ_FIELD_RANGE(float, playerKnockSeconds, 0.22f, "Duration", 0.0f, 1.5f)
    FBZZ_TOOLTIP("押される時間。長いほど «操作を取り上げられた» に寄るので短く")

    /// 盤面を経由しない衝突の報告口。転がっている Roller が他の敵を轢いたときのように、
    /// «引力で飛んだわけではないが、確かに衝突が起きた» 経路が通る。
    ///
    /// WHY 別の入口を作らず ResolveImpact へ流すか: 連鎖の数え方・倍率・音・撃破の
    ///     集計はすべて «衝突が起きた» に紐づいている。別入口を作ると、連鎖を
    ///     1 行変えるたびに 2 箇所を直すことになり、片方だけ仕様から外れる。
    void ReportImpact(const PolarityImpact& impact) { ResolveImpact(impact); }

    /// 盤面を経由しない敵へのダメージ。床ハザードのような環境ダメージが通る。
    /// 倒したら true。撃破数の集計と撃破コアの生成は衝突経路と同じ規則を通る。
    bool DamageEnemyDirect(GameObject* target, int amount);

    /// character の身に起きたことを、そのキャラクターの反応へ配る。
    ///
    /// 被弾と撃破はここが自分で流すので、外から呼ぶのは「見つけた」「攻撃を出した」など
    /// ダメージを伴わない出来事だけでよい。
    ///
    /// WHY 反応先を呼び出し元に選ばせないか:
    ///   同じ «倒れた» に対して、表情・アニメーション・HUD・ボイスがそれぞれ反応する。
    ///   呼び出し元が反応先を名指しすると、反応を 1 つ足すたびに敵 AI とプレイヤーの
    ///   両方を触ることになり、片方だけ足し忘れた種類の敵ができる。
    ///   誰に何が起きたかを言うのは呼び出し元、どこへ配るかを決めるのはここ。
    void Notify(GameObject* character, CharacterEvent event) const;

    void OnStart() override;
    void OnUpdate() override;
    void OnDestroy() override;

private:
    static inline CombatManagerComponent* s_instance = nullptr;

    /// 盤面の購読。開始順に依存しないよう、繋がるまで毎フレーム試す。
    void EnsureSubscribed();
    void ResolveImpact(const PolarityImpact& impact);
    /// 敵 1 体ぶんのダメージ適用。撃破したら戦果を進める。
    /// @param chainMultiplier 連鎖の深さから来る倍率。1.0 は «連鎖の恩恵なし»。
    void DamageEnemy(GameObject* target, const PolarityImpact& impact, float chainMultiplier);
    /// 撃破の後始末 (戦果・撃破コア・進行への通知)。倒した経路によらず同じ道を通す。
    void RegisterDefeat(GameObject* target, Polarity fallbackPolarity);

    /// 撃破コアの生成待ち。
    ///
    /// WHY その場で作らないか: 撃破は盤面の衝突を解決している最中に起きる。あちらは
    ///     GameObject* を持ったまま回っているので、途中で GameObject を増やすと
    ///     配列の再確保でその参照が無効になりうる。
    ///     «倒れた» を覚えておいて、次の OnUpdate で置く。
    struct PendingCore {
        Vector3  point{};
        Polarity polarity = Polarity::None;
    };
    void SpawnPendingCores();
    /// 空いている枠を 1 つ返す。上限まで埋まっていたら最も古い枠を奪う。
    [[nodiscard]] GameObject* AcquireCoreSlot();

    std::vector<PendingCore> m_pendingCores;
    std::vector<EntityRef>   m_coreSlots;
    int  m_coreNext = 0;
    bool m_warnedNoCoreVfx = false;
    bool m_warnedNoPlayerTarget = false;
    /// 受け手の検算を始めるまでの猶予 (秒)。スクリプト間のライフサイクルの
    /// 前後関係を吸収する。
    static constexpr float kTargetGraceSeconds = 0.5f;
    float m_targetGrace = 0.0f;

    bool m_subscribed = false;
    // 「盤面が居ない」の報告口。購読は繋がるまで毎フレーム試すので、
    // 最初の数フレームで繋がらないのは並び順の都合でしかない (ManagerWatch.hpp)。
    ManagerWatch m_fieldWatch;

    int m_kills = 0;
    int m_pushKills = 0;
    int m_enemyImpacts = 0;
    int m_anchorImpacts = 0;
    int m_damageToEnemies = 0;
    int m_damageToPlayer = 0;

    int   m_chain = 0;
    int   m_bestChain = 0;
    float m_chainRemaining = 0.0f;
    /// 最後に連鎖を 1 つ進めたフレーム。同じフレームの 2 発目以降は数えない。
    std::uint64_t m_chainFrame = 0;
    /// この連鎖で当たった回数。同じフレームでも 1 発ごとに進む (倍率の元)。
    int m_chainHits = 0;
};

FBZZ_REFLECT(CombatManagerComponent)

inline void CombatManagerComponent::OnStart()
{
    if (s_instance && s_instance != this) {
        debug.LogWarning("CombatManagerComponent: another instance is already active. "
                         "The most recently started one takes over.");
    }
    s_instance = this;

    m_kills = 0;
    m_pushKills = 0;
    debugPushKills = 0;
    m_enemyImpacts = 0;
    m_anchorImpacts = 0;
    m_damageToEnemies = 0;
    m_damageToPlayer = 0;
    m_chain = 0;
    m_bestChain = 0;
    m_chainRemaining = 0.0f;
    m_chainFrame = 0;
    m_chainHits = 0;
    m_subscribed = false;
    m_fieldWatch.Reset();

    m_pendingCores.clear();
    m_coreSlots.clear();
    m_coreNext        = 0;
    m_warnedNoCoreVfx = false;
    debugCoresAlive   = 0;

    // 連鎖の読み上げは画面の出来事であって空間の出来事ではないので UI バスの 2D。
    se::EnsureSource(scene, "UI");

    // 枠を先に積む。戦闘中に GameObject を作ると配列の再確保が走り、その 1 フレームだけ
    // 盤面が固まる。
    //
    // WHY 使うときに 1 つずつではなく «全部» 作るか:
    //   枠には PolarityTargetComponent を後から足している。足した Script の OnStart は
    //   次のスクリプトフェーズで走り、その中で ClearPolarity() が呼ばれる。使う瞬間に
    //   作って同じフレームで極を乗せると、直後の OnStart がそれを消してしまい、
    //   生まれた次のフレームに «極が切れた» と判断して自分で畳む。開始時に作り切れば、
    //   極を乗せるときには必ず OnStart を抜けている。
    if (spawnKillCores) {
        const int capacity = std::max(maxKillCores, 1);
        for (int i = 0; i < capacity; ++i) {
            GameObject* slot = AcquireCoreSlot();
            if (!slot) break;
            slot->SetActive(false);
        }
        m_coreNext = 0;
    }

    EnsureSubscribed();
}

inline void CombatManagerComponent::OnDestroy()
{
    // 枠はルートに置いた GameObject なので、このスクリプトと一緒には消えない。
    //
    // WHY 畳まないと何が起きるか: 撃破コアの枠は粒を «ループで» 出し続ける設定に
    //     なっている (KillCoreComponent::ConfigureEmitter)。畳まずに残すと、Play を
    //     抜けた後も編集中の画面で粒が湧き続ける。VfxManagerComponent が
    //     ReleaseSlots() で同じ後始末をしているのと同じ理由。
    for (const EntityRef& slot : m_coreSlots)
        if (GameObject* object = slot.Resolve(scene)) scene.Destroy(*object);
    m_coreSlots.clear();
    m_pendingCores.clear();

    if (s_instance == this) s_instance = nullptr;
}

inline GameObject* CombatManagerComponent::AcquireCoreSlot()
{
    const int capacity = std::max(maxKillCores, 1);

    if (static_cast<int>(m_coreSlots.size()) < capacity) {
        // WHY 名前で拾い直すか: スクリプト DLL をリロードするとこの Script は作り直され、
        //     EntityRef は空に戻る。枠の GameObject は Scene 側に残っているので、
        //     拾わずに作るとリロードのたびに枠が capacity 個ずつ増える。
        const std::string name = "KillCore_" + std::to_string(m_coreSlots.size());

        // WHY .vfx を展開せず素の GameObject を作るか: コアの絵はパーティクル 1 種類
        //     だけで出来ていて (PolarityCore.hlsl)、その設定は KillCoreComponent が
        //     極に合わせて毎回組み直す。.vfx を挟むと «アセットに書いてある設定» と
        //     «スクリプトが上書きする設定» の 2 つが同じ値を持つことになる。
        GameObject* object = scene.Find(name);
        if (!object) {
            GameObject& created = scene.Create(name);
            created.runtimeGenerated = true;
            object = &created;
        }

        if (!scene.GetScript<PolarityTargetComponent>(object))
            object->AddScript<PolarityTargetComponent>();
        if (!scene.GetScript<KillCoreComponent>(object))
            object->AddScript<KillCoreComponent>();

        // WHY ここで «性格» まで決めるか: 足した Script の OnStart は次のフェーズで走り、
        //     PolarityTargetComponent はそこで «調整値が無い / 発光を書ける材質が無い» を
        //     エラーとして報告する。コアは粒だけで出来ていて材質を持たないので、
        //     selfDriven を立てる «前» に OnStart を通すと、枠の数だけ嘘のエラーが並ぶ。
        auto* field = PolarityFieldComponent::Instance();
        if (auto* target = scene.GetScript<PolarityTargetComponent>(object)) {
            if (field) target->tuning.ref = field->tuning.ref;
            target->polarityClass  = PolarityClass::Pillar;
            target->isAnchor       = true;
            target->selfDriven     = true;
            target->acceptsPaint   = true;
            target->outlineEnabled = false;
        }
        if (auto* core = scene.GetScript<KillCoreComponent>(object)) {
            if (field) core->tuning.ref = field->tuning.ref;
        }

        m_coreSlots.push_back(EntityRef{ object->GetID() });
        return object;
    }

    // 埋まっているので最も古い枠を奪う。next は常に «次に使う = 最も古い» を指す。
    EntityRef& slot = m_coreSlots[static_cast<std::size_t>(m_coreNext)];
    m_coreNext = (m_coreNext + 1) % static_cast<int>(m_coreSlots.size());
    return slot.Resolve(scene);
}

inline void CombatManagerComponent::SpawnPendingCores()
{
    // 生きている数はここで数え直す。コアは自分で畳むので、こちらは «置く» だけを持つ。
    int alive = 0;
    for (const EntityRef& slot : m_coreSlots) {
        const GameObject* object = slot.Resolve(scene);
        if (object && object->activeInHierarchy()) ++alive;
    }
    debugCoresAlive = alive;

    if (m_pendingCores.empty()) return;

    if (!spawnKillCores) {
        m_pendingCores.clear();
        return;
    }

    auto* field = PolarityFieldComponent::Instance();
    for (const PendingCore& pending : m_pendingCores) {
        GameObject* object = AcquireCoreSlot();
        if (!object) {
            if (!m_warnedNoCoreVfx) {
                m_warnedNoCoreVfx = true;
                debug.LogError("CombatManagerComponent could not create a kill core slot. "
                               "Multi-body convergence will almost never fire.");
            }
            break;
        }

        object->transform.position      = pending.point;
        object->transform.worldPosition = pending.point;
        object->SetActive(true);

        if (auto* core = scene.GetScript<KillCoreComponent>(object)) {
            // 調整値はコア自身では解決できない。盤面が持っている 1 枚を回す。
            if (field) core->tuning.ref = field->tuning.ref;
            core->Configure(pending.polarity);
        }
    }
    m_pendingCores.clear();
}

inline void CombatManagerComponent::EnsureSubscribed()
{
    if (m_subscribed) return;

    // WHY 繋がるまで試し続けるか: 盤面とこのマネージャーは別の GameObject に居るので、
    //     どちらの OnStart が先に走るかはシーンの並び次第になる。開始時の 1 回だけで
    //     諦めると、並べ替えただけでダメージが入らなくなる。
    auto* field = PolarityFieldComponent::Instance();
    if (!field) return;

    field->onImpact = [this](const PolarityImpact& impact) { ResolveImpact(impact); };
    m_subscribed = true;
}

inline void CombatManagerComponent::OnUpdate()
{
    EnsureSubscribed();

    // ── 受け手の検算 ────────────────────────────────────────────────────────
    // WHY 1 フレーム目で判定しないか:
    //   このエンジンは «全員の OnStart → 全員の OnUpdate» の順では回らない。実測では
    //   CombatManager の最初の OnUpdate が PlayerComponent の OnStart より先に走り、
    //   その時点の名簿はまだ空だった。1 フレーム目を見て報告すると
    //   «まだ名乗っていないだけ» を欠陥として出してしまう。
    //
    // WHY 攻撃が当たるまで待たないか:
    //   DamagePlayer 側の報告は «敵が初めて殴った瞬間» まで出ない。数分戦ってから
    //   «ずっと無敵だった» と判るのでは遅い。猶予を置いて 1 度だけ確かめる。
    if (!m_warnedNoPlayerTarget && m_targetGrace < kTargetGraceSeconds) {
        m_targetGrace += std::max(time.UnscaledDeltaTime(), 0.0f);
        if (m_targetGrace >= kTargetGraceSeconds) {
            GameObject* player = scene.FindWithTag("Player");
            if (!player) {
                m_warnedNoPlayerTarget = true;
                debug.LogError("CombatManagerComponent: tag=Player の GameObject が無い。"
                               "敵の攻撃は誰にも当たらない。");
            } else if (!IDamageable::Of(player)) {
                m_warnedNoPlayerTarget = true;
                debug.LogError("CombatManagerComponent: '" + player->name
                               + "' が IDamageable として名乗っていない。"
                                 "PlayerComponent::OnStart の IDamageable::Bind() を"
                                 "確認すること。このままでは敵の攻撃が一切通らない。");
            }
        }
    }

    SpawnPendingCores();
    debugFaces = EyeSpriteComponent::RegisteredCount();

    // WHY 実時間で数えるか: 連鎖の途中は必ずヒットストップが掛かる。縮んだ時間で
    //     数えると、派手に決まった連鎖ほど猶予が伸びて別物の判定になる。
    if (m_chainRemaining > 0.0f) {
        m_chainRemaining -= std::max(time.UnscaledDeltaTime(), 0.0f);
        if (m_chainRemaining <= 0.0f) {
            m_chainRemaining = 0.0f;
            m_chain = 0;
            m_chainHits = 0;
            debugChainHits = 0;
            debugChain = 0;
        }
    }

    // 連鎖が続いている間だけ盤面の届く距離を伸ばす。数えているのはこちらなので、
    // 引かせるのではなく押し込む (PolarityFieldComponent の SetChainWindow の WHY)。
    if (auto* field = PolarityFieldComponent::Instance())
        field->SetChainWindow(ChainRemaining01());

    // 盤面がシーンに 1 つも無いなら、極性衝突のダメージは永久に入らない。
    // 黙って進むと「当てても減らない」だけが残るので、1 度だけ名指しで止める。
    // ただし最初の数フレームで繋がらないのは、盤面の OnStart がこちらより後ろに
    // 並んでいるだけのことが多い。猶予を過ぎても繋がらないときだけ出す。
    if (m_fieldWatch.ShouldReport(m_subscribed)) {
        debug.LogError("CombatManagerComponent found no PolarityFieldComponent in the scene. "
                       "Polarity impacts will never deal damage.");
    }
}

inline void CombatManagerComponent::Notify(GameObject* character, CharacterEvent event) const
{
    if (!character) return;

    // 反応を持たないキャラクターは黙って素通りさせる。目を付けていないだけで
    // 敵が 1 種類まるごと «壊れている» ように見えてはいけない。
    if (auto* eyes = EyeSpriteComponent::For(character)) eyes->React(event);
}

inline bool CombatManagerComponent::DamagePlayer(GameObject* player, int amount,
                                                 const Vector3* source)
{
    // WHY PlayerComponent ではなく IDamageable で引くか: 「殴られる側」であることだけが
    //     ここでの関心で、それがプレイヤーかどうかは知らなくてよい。将来プレイヤーが
    //     乗り物に乗る / 分身を出すといった構成になっても、この関数は変わらない。
    //
    // WHY scene.GetScript<IDamageable>() ではないか: この環境では横断インターフェースで
    //     引くと必ず nullptr が返る (基底たどりが効いていない。IDamageable::Of を参照)。
    //     ここが空振りすると «敵の攻撃が一切通らない» という形でしか症状が出ないので、
    //     確実に引ける名簿の方を使う。
    auto* target = IDamageable::Of(player);
    if (!target) {
        // WHY ここだけ名指しで報告するか: 敵の攻撃が «当たっているのに減らない» とき、
        //     画面には «避けられている» としか出ない。受け手が居ないのか無敵で
        //     弾かれたのかは、この 1 行が無いと攻撃側からもプレイヤー側からも見えない。
        if (!m_warnedNoPlayerTarget) {
            m_warnedNoPlayerTarget = true;
            debug.LogError("CombatManagerComponent: the player object has no IDamageable "
                           "(PlayerComponent). Enemy attacks can never deal damage. "
                           "IDamageable::Bind() を OnStart で呼んでいるか確認すること。");
        }
        return false;
    }

    const int before = target->CurrentHealth();
    if (!target->ApplyDamage(std::max(amount, 1))) return false;

    // 無敵時間で弾かれた分を数えないよう、実際に減った量だけを積む。
    m_damageToPlayer += std::max(before - target->CurrentHealth(), 0);
    debugDamageToPlayer = m_damageToPlayer;

    // 押しは «通った» ときだけ。無敵で弾いた一撃でも押すと、避けているのに
    // 位置だけ持っていかれる。押し方を知っているのは殴られた側 (IDamageable)。
    if (source && playerKnockSpeed > 0.0f)
        target->ApplyKnockback(*source, playerKnockSpeed, playerKnockSeconds);

    Notify(player, target->IsAlive() ? CharacterEvent::Hurt : CharacterEvent::Defeated);
    return true;
}

inline void CombatManagerComponent::DamageEnemy(GameObject* target, const PolarityImpact& impact,
                                                 float chainMultiplier)
{
    auto* health = scene.GetScript<EnemyHealthComponent>(target);
    if (!health) return;

    const int before = health->Current();
    const bool defeated = health->TakeImpact(impact, chainMultiplier);
    m_damageToEnemies += std::max(before - health->Current(), 0);
    debugDamageToEnemies = m_damageToEnemies;

    Notify(target, defeated ? CharacterEvent::Defeated : CharacterEvent::Hurt);
    if (!defeated) return;

    // 反発で飛んだ末の撃破は «押し込み» として別に数える。ランク評価が引きと押しを
    // 別の軸に置いているので、どちらで倒したかを取り違えると評価が片方へ寄る。
    if (impact.fromRepulse) AddPushKill();

    RegisterDefeat(target, impact.moverPolarity);
}

inline bool CombatManagerComponent::DamageEnemyDirect(GameObject* target, int amount)
{
    auto* health = scene.GetScript<EnemyHealthComponent>(target);
    if (!health || !health->IsAlive()) return false;

    const int before = health->Current();
    const bool defeated = health->ApplyDamage(std::max(amount, 1));
    m_damageToEnemies += std::max(before - health->Current(), 0);
    debugDamageToEnemies = m_damageToEnemies;

    Notify(target, health->IsAlive() ? CharacterEvent::Hurt : CharacterEvent::Defeated);
    if (!defeated) return false;

    RegisterDefeat(target, Polarity::None);
    return true;
}

// 撃破の «後始末» を 1 箇所へ集める。衝突で倒した経路と、ハザードのように盤面を
// 経由しない経路で、数え方とコアの残り方が違ってはいけない。
inline void CombatManagerComponent::RegisterDefeat(GameObject* target, Polarity fallbackPolarity)
{
    ++m_kills;
    debugKills = m_kills;

    // 倒した場所へ的を残す。倒すたびに次の一手の燃料が生まれるので、
    // «敵が 1 体になった瞬間に手詰まり» が起きなくなる。
    //
    // WHY 極を «ぶつけた側» から取るか: 倒された側の極は衝突で使い切られている
    //     (PolarityFieldComponent の consumePolarityOnImpact)。残っていればそれを、
    //     消えていれば当てた側の極を継ぐ。どちらも «この撃破に使われた極» になる。
    if (spawnKillCores && target &&
        static_cast<int>(m_pendingCores.size()) < std::max(maxKillCores, 1)) {
        Polarity polarity = Polarity::None;
        if (const auto* victim = scene.GetScript<PolarityTargetComponent>(target))
            polarity = victim->Current();
        if (polarity == Polarity::None) polarity = fallbackPolarity;

        if (polarity != Polarity::None) {
            Vector3 point = target->transform.worldPosition;
            point.y += std::max(killCoreHeight, 0.0f);
            m_pendingCores.push_back({ point, polarity });
        }
    }

    if (onEnemyDefeated) onEnemyDefeated();
}

inline void CombatManagerComponent::ResolveImpact(const PolarityImpact& impact)
{
    if (impact.struckIsAnchor) {
        ++m_anchorImpacts;
        debugAnchorImpacts = m_anchorImpacts;
    } else {
        ++m_enemyImpacts;
        debugEnemyImpacts = m_enemyImpacts;
    }

    // 猶予の中に次が来れば同じ連鎖。1 回の仕掛けから何手続いたかを数える。
    //
    // WHY 1 フレームに何度ぶつかっても 1 つしか数えないか:
    //   7.9 の集束は 4 体が同じ点へ «同時に» 飛び込む。1 衝突 1 連鎖で数えると、
    //   それだけで深さが 4 まで跳ね、«順番に繋いだ» のと «一度に潰した» のが
    //   同じ数字になる。連鎖の深さが表すべきなのは時間的な連なりの方なので、
    //   同じフレームに畳み込まれた分は 1 手として数える。
    //   (猶予そのものは毎回張り直す。同時多発でも «続いている» ことは確かなため)
    const std::uint64_t frame = time.FrameCount();
    if (frame != m_chainFrame) {
        m_chainFrame = frame;
        ++m_chain;
        m_bestChain    = std::max(m_bestChain, m_chain);
        debugChain     = m_chain;
        debugBestChain = m_bestChain;
    }
    // 倍率が見るのはこちら。同じフレームに 4 体落ちれば 4 つ進む。
    ++m_chainHits;
    debugChainHits = m_chainHits;
    m_chainRemaining = std::max(chainSeconds, 0.0f);

    // WHY 2 連目から鳴らすか: 1 回目はまだ «仕掛けた» だけで、連鎖はしていない。
    //     衝突そのものの手応えは ImpactFeedbackManager が強さで返しているので、
    //     ここで 1 回目にも鳴らすと同じ出来事に音が 2 つ乗る。«続いた» ことだけを言う。
    //
    // WHY 進行の音と同じ UI バスへ出すか: これは盤面のどこかで起きた音ではなく、
    //     プレイヤーの組み立てが成立したという読み上げ。距離で減衰すると、
    //     遠くへ組んだ長い連鎖ほど聞こえない、という逆立ちが起きる。
    if (m_chain >= 2) {
        se::Play(audio, m_chain >= std::max(highChainCount, 2) ? se::kUiComboHigh
                                                               : se::kUiCombo);
    }

    // WHY 倍率を «叩きつけた先» にしか乗せないか:
    //   雑魚は 100 HP しかなく、被弾値も 55〜100。倍率を乗せても «1 体倒す» で
    //   頭打ちになり、9 連鎖まで伸ばしたエネルギーの 3/4 が 100 HP の壁で捨てられる。
    //   受け止められる HP を持っているのは動かない側 (ボス 800・柱) だけなので、
    //   伸ばした分をそこへ集める。«長く繋いで最後をボスへ» がそのまま目的になり、
    //   雑魚の処理は連鎖の深さに関係なく «1〜2 発» のまま読める。
    //
    // 飛んだ側は自分から突っ込んだ側なので、深さに関わらず素の値。
    DamageEnemy(impact.mover, impact, 1.0f);

    // WHY 受け手がアンカーでも減らすか: 7.6 で柱を廃した後、動かない側に残ったのは
    //     «重すぎて飛ばない敵» (Roller) と撃破コアだけになった。10.1 は Roller を
    //     「衝突 2 回で撃破」と決めているので、的であることはダメージを切る理由にならない。
    //     地形は EnemyHealthComponent を持たないため、DamageEnemy が自然に空振りする。
    //     正面衝突で mover == struck になる経路だけは弾く。
    if (impact.struck != impact.mover) {
        DamageEnemy(impact.struck, impact,
                    impact.struckIsAnchor ? ChainDamageMultiplier() : 1.0f);
    }
}

} // namespace sandbox
