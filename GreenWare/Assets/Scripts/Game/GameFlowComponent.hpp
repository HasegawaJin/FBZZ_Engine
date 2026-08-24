/// @file GameFlowComponent.hpp
/// @brief 敵全滅・プレイヤー死亡・HUD・リザルト遷移といったゲーム進行を統括する
/// @author Hasegawa Jin
/// @date 2026-08-22
///
/// WHY 戦闘の解決を持たないか:
///   衝突をダメージへ変換するのは CombatManagerComponent の担当。ダメージ式は 18.2 が
///   未決で何度も触るのに対し、ここが持つ「いつリザルトへ行くか」はほとんど変わらない。
///   同居させると、片方を触るたびにもう片方を読む必要が出る。
///   戦果 (撃破数・衝突回数) は取得用の API で受け取り、リザルトへ渡すだけにする。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/GameResultState.hpp>
#include <Scripts/Player/PlayerComponent.hpp>
#include <Scripts/Utils/ManagerWatch.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using fbzz::Time;

namespace sandbox {

class GameFlowComponent : public Script {
    FBZZ_SCRIPT(GameFlowComponent)

public:
    FBZZ_GROUP("Scenes")
    FBZZ_FIELD(std::string, resultScene, "Result", "Result Scene")
    FBZZ_FIELD_RANGE(float, endDelay, 0.8f, "End Delay", 0.0f, 5.0f)

    // WHY 体力と銃の表示を持たないか:
    //   体力は PlayerHealthBarComponent、銃のクールダウンは PolarityGunHudComponent が
    //   バーとして出している。同じ値を進行側でも文字にすると、片方の書式や色を変えた
    //   ときにもう片方だけが取り残される。ここが出すのは進行 (残り敵数と目的) だけ。
    FBZZ_GROUP("HUD Names")
    FBZZ_FIELD(std::string, enemyTextName, "HUD_Enemies", "Enemy Text")
    FBZZ_FIELD(std::string, objectiveTextName, "HUD_Objective", "Objective Text")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugEnemies, 0, "Enemies")
    FBZZ_FIELD_READ_ONLY(float, debugElapsed, 0.0f, "Elapsed")

    void OnStart() override;
    void OnUpdate() override;

private:
    void BeginEnd(bool victory);
    void RefreshHud(int enemiesAlive);
    [[nodiscard]] int CountEnemies() const;

    // WHY 保持せず毎回引くか: マネージャーは別の子オブジェクトに居るので、
    //     どちらの OnStart が先に走るかはシーンの並び次第になる。開始時に 1 度
    //     掴んで持ち続けると、並び順を変えただけで戦果が丸ごと 0 になる。
    [[nodiscard]] CombatManagerComponent* Combat() const
    {
        return CombatManagerComponent::Instance();
    }

    PlayerComponent* m_player = nullptr;
    // 「戦闘が居ない」の報告口。最初のフレームだけ空なのは並び順の都合なので、
    // 猶予を過ぎても見つからないときだけ出す (ManagerWatch.hpp)。
    ManagerWatch m_combatWatch;
    bool m_sawEnemy = false;
    bool m_ending = false;
    bool m_victory = false;
    float m_endRemaining = 0.0f;
    float m_elapsed = 0.0f;
};

FBZZ_REFLECT(GameFlowComponent)

inline void GameFlowComponent::OnStart()
{
    m_elapsed = 0.0f;
    m_ending = false;

    // 進行の音は画面の出来事であって空間の出来事ではないので UI バスの 2D。
    se::EnsureSource(scene, "UI");
    se::Play(audio, se::kUiWaveStart);

    m_combatWatch.Reset();
    // WHY ここで敵を数えないか: CombatManagerComponent の OnStart がこのスクリプトより
    //     後ろに並んでいると Instance() がまだ空で、CountEnemies() は必ず 0 を返す。
    //     「開始時に敵が居たか」を 0 で確定させると、実際には敵が居るのに
    //     居なかったことになる。数えるのは OnUpdate に任せる (下で毎フレーム更新する)。
    m_sawEnemy = false;

    if (GameObject* player = scene.FindWithTag("Player")) {
        m_player = scene.GetScript<PlayerComponent>(player);
        if (m_player)
            m_player->SetOnDeath([this]() { BeginEnd(false); });
    }

    if (!m_player)
        debug.LogError("GameFlowComponent requires a PlayerComponent on the Player-tagged object.");
}

inline int GameFlowComponent::CountEnemies() const
{
    // 数える規則は戦闘側の知識なので委譲する。未設定でも進行が止まらないよう 0 を返す。
    auto* combat = Combat();
    return combat ? combat->CountEnemiesAlive() : 0;
}

inline void GameFlowComponent::BeginEnd(bool victory)
{
    if (m_ending) return;
    m_ending = true;
    m_victory = victory;
    m_endRemaining = std::max(endDelay, 0.0f);

    // 決着はリザルト画面へ移る前に鳴らす。endDelay の間、画面には結果が出ているのに
    // 音だけシーン遷移まで来ない、という間ができないようにする。
    se::Play(audio, victory ? se::kUiGameClear : se::kUiGameOver);
}

inline void GameFlowComponent::RefreshHud(int enemiesAlive)
{
    if (GameObject* text = scene.Find(enemyTextName))
        ui.SetText(text, "ENEMIES " + std::to_string(enemiesAlive));
    if (GameObject* text = scene.Find(objectiveTextName))
        ui.SetText(text, m_ending ? (m_victory ? "AREA CLEAR" : "SYSTEM DOWN")
                                  : "RED +  BLUE -  COLLIDE ENEMIES");
}

inline void GameFlowComponent::OnUpdate()
{
    // 戦闘が居ないと敵は永遠に減らず、勝利条件が来ない。1 度だけ名指しで止める。
    // WHY 初回フレームで判定しないか: ScriptSystem は GameObject ごとに
    //     OnAwake → OnStart → OnUpdate をまとめて回す。CombatManager がこのスクリプトより
    //     後ろに並んでいると、最初の OnUpdate の時点ではまだ OnStart が走っておらず
    //     Instance() が空になる。そこで確定させると、正しく置いてあるシーンでも
    //     毎回このエラーが出る (実際そうなっていた)。猶予は ManagerWatch が持つ。
    if (m_combatWatch.ShouldReport(Combat() != nullptr)) {
        debug.LogError("GameFlowComponent found no CombatManagerComponent in the scene "
                       "(no damage is applied and the wave never clears).");
    }

    if (!m_ending) m_elapsed += Time::deltaTime;
    debugElapsed = m_elapsed;

    const int enemies = CountEnemies();
    debugEnemies = enemies;
    if (enemies > 0) m_sawEnemy = true;
    if (!m_ending && m_sawEnemy && enemies == 0) BeginEnd(true);
    RefreshHud(enemies);

    if (!m_ending) return;
    m_endRemaining -= Time::deltaTime;
    if (m_endRemaining > 0.0f) return;

    const auto* combat = Combat();
    GameResultState::victory = m_victory;
    GameResultState::clearSeconds = m_elapsed;
    GameResultState::defeatedEnemies = combat ? combat->Kills() : 0;
    GameResultState::enemyImpacts = combat ? combat->EnemyImpacts() : 0;
    GameResultState::anchorImpacts = combat ? combat->AnchorImpacts() : 0;
    scene.LoadScene(resultScene);
}

} // namespace sandbox
