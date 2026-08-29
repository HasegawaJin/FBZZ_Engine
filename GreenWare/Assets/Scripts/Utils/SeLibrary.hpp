/// @file    SeLibrary.hpp
/// @brief   SE アセットの所在表と、変奏を 1 つ選ぶ規則
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// WHY Inspector のフィールドではなくコードの表か:
///   素材は 1 つの出来事に対して 2〜6 個の変奏 (_01.._06) で入っている。
///   足音だけで 10 個、被弾で 3 個あり、これを Inspector のスロットに並べると
///   1 スクリプトに 10 行の音声フィールドが生え、シーンを触るたびに割り当て直しになる。
///   「どの出来事にどの音を使うか」は素材のファイル名がすでに宣言しているので、
///   その対応をコード側の 1 箇所に持ち、Inspector は例外の上書きだけを受け持つ。
///
/// WHY スクリプト側で AudioSource を用意するか:
///   SE を鳴らすスクリプトは 10 個近くあり、その全部の GameObject へ手で
///   AudioSource を置いて回ると、1 つ置き忘れた場所だけが無音になる。
///   しかも症状が「鳴らない」だけなので、置き忘れに気付く手段が無い
///   (実際 SE 実装前のシーンには AudioSource も AudioListener も 1 つも無く、
///   既存の PlayOneShot 9 箇所はすべて無言で捨てられていた)。
///   シーンに置いてあればそれを尊重し、無いときだけ足す。
#pragma once

#include <Engine/Scene/Components/AudioListenerComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

using namespace fbzz::scene;

namespace sandbox::se {

/// 同じ出来事に対する変奏の集合。
///
/// WHY 束ごとに const を付けないか: 名前空間スコープの const 変数は内部リンケージに
///     なり、翻訳単位ごとに別の実体になる。連続を避けるための last が TU ごとに
///     分かれても壊れはしないが、同じ束が 2 つある状態を作る理由が無い。
struct Bank {
    std::span<const std::string_view> variants;
    /// 直前とその 1 つ前に選んだ添字。連続を避けるためだけの状態なので const 越しに書き換える。
    mutable int last = -1;
    mutable int prev = -1;
    /// Next() の巡回位置。Pick() とは別に持つ (順送りと抽選は同じ束でも用途が違う)。
    mutable int cursor = -1;

    [[nodiscard]] bool             Empty() const { return variants.empty(); }
    [[nodiscard]] std::string_view Pick()  const;
    /// 並び順のまま次を返す。素材が «順に運ぶ» 前提で作られている束に使う
    /// (Boss の 4 脚クロールは 01→04 が 1 周期ぶんの接地なので、抽選すると歩容が崩れる)。
    [[nodiscard]] std::string_view Next() const;
    /// 変奏を選ばず先頭を返す。ループ音のように「毎回同じでよい」ものに使う。
    [[nodiscard]] std::string_view First() const
    { return variants.empty() ? std::string_view{} : variants[0]; }
};

namespace detail {

/// 変奏選びの軽い乱数。
///
/// WHY Script の random プロキシを使わないか: Bank は Script を持たない自由関数から
///     選ばれる。プロキシを引き回すと、音を鳴らすすべての関数が Script を受け取る
///     羽目になる。SE の並び順に再現性は要らないので、ここで完結させる。
[[nodiscard]] inline std::uint32_t NextRandom()
{
    static std::uint32_t state = 0x9E3779B9u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

} // namespace detail

inline std::string_view Bank::Pick() const
{
    if (variants.empty()) return {};
    const int count = static_cast<int>(variants.size());
    if (count == 1) return variants[0];

    int index = static_cast<int>(detail::NextRandom() % static_cast<std::uint32_t>(count));

    // WHY 直前«2 つ»を外すか: 一様乱数だと 4 変奏でも 4 回に 1 回は同じ音が続く。
    //     足音のように短い間隔で鳴るものでは、その 1 回が「使い回し」として耳に付く。
    //     素材側の README (Assets/Sound/SE/README) も «直前 2 つと同じ番号を引かない
    //     シャッフルで選ぶ» を前提に変奏を層化してあり、直前 1 つだけでは
    //     A-B-A-B の往復が残る。
    //     4 変奏未満で 2 つ外すと候補が 1 つ以下に潰れて «必ずこの順» になるので、
    //     そこは直前 1 つに緩める。
    const bool avoidTwo = count >= 4;
    for (int guard = 0; guard < count; ++guard) {
        if (index != last && !(avoidTwo && index == prev)) break;
        index = (index + 1) % count;
    }

    prev = last;
    last = index;
    return variants[index];
}

inline std::string_view Bank::Next() const
{
    if (variants.empty()) return {};
    cursor = (cursor + 1) % static_cast<int>(variants.size());
    return variants[cursor];
}


// ── Player ──────────────────────────────────────────────────────────────────
// WHY 変奏を 1 つも省かないか:
//   足音の変奏はピッチ違いではなく «接地の構造» ごと別に合成されていて、素材側は
//   全変奏に渡ってパラメーターを層化してある (範囲を変種数で等分して 1 つずつ配る)。
//   走り 8 / 歩き 6 のうち何本かを外すと、その層化が崩れて残った変奏だけが偏り、
//   «作り分けたのに似て聞こえる» に戻る。数を減らすなら素材から作り直す側の仕事。
inline constexpr std::string_view kPlayerFootstepWalkPaths[] = {
    "Assets/Sound/SE/Player/SE_PL_Footstep_Walk_01.wav",
    "Assets/Sound/SE/Player/SE_PL_Footstep_Walk_02.wav",
    "Assets/Sound/SE/Player/SE_PL_Footstep_Walk_03.wav",
    "Assets/Sound/SE/Player/SE_PL_Footstep_Walk_04.wav",
    "Assets/Sound/SE/Player/SE_PL_Footstep_Walk_05.wav",
    "Assets/Sound/SE/Player/SE_PL_Footstep_Walk_06.wav",
};
inline constexpr std::string_view kPlayerFootstepRunPaths[] = {
    "Assets/Sound/SE/Player/SE_PL_Footstep_Run_01.wav",
    "Assets/Sound/SE/Player/SE_PL_Footstep_Run_02.wav",
    "Assets/Sound/SE/Player/SE_PL_Footstep_Run_03.wav",
    "Assets/Sound/SE/Player/SE_PL_Footstep_Run_04.wav",
    "Assets/Sound/SE/Player/SE_PL_Footstep_Run_05.wav",
    "Assets/Sound/SE/Player/SE_PL_Footstep_Run_06.wav",
    "Assets/Sound/SE/Player/SE_PL_Footstep_Run_07.wav",
    "Assets/Sound/SE/Player/SE_PL_Footstep_Run_08.wav",
};
inline constexpr std::string_view kPlayerDodgePaths[] = {
    "Assets/Sound/SE/Player/SE_PL_Dodge_01.wav",
    "Assets/Sound/SE/Player/SE_PL_Dodge_02.wav",
    "Assets/Sound/SE/Player/SE_PL_Dodge_03.wav",
};
inline constexpr std::string_view kPlayerDodgeEndPaths[] = {
    "Assets/Sound/SE/Player/SE_PL_Dodge_End.wav",
};
inline constexpr std::string_view kPlayerLandingPaths[] = {
    "Assets/Sound/SE/Player/SE_PL_Landing_01.wav",
    "Assets/Sound/SE/Player/SE_PL_Landing_02.wav",
};
inline constexpr std::string_view kPlayerDamagedPaths[] = {
    "Assets/Sound/SE/Player/SE_PL_Damaged_01.wav",
    "Assets/Sound/SE/Player/SE_PL_Damaged_02.wav",
    "Assets/Sound/SE/Player/SE_PL_Damaged_03.wav",
};
inline constexpr std::string_view kPlayerDeathPaths[] = {
    "Assets/Sound/SE/Player/SE_PL_Death.wav",
};
inline constexpr std::string_view kPlayerServoTurnPaths[] = {
    "Assets/Sound/SE/Player/SE_PL_Servo_Turn_01.wav",
    "Assets/Sound/SE/Player/SE_PL_Servo_Turn_02.wav",
    "Assets/Sound/SE/Player/SE_PL_Servo_Turn_03.wav",
};
inline constexpr std::string_view kPlayerWeaponDeployPaths[] = {
    "Assets/Sound/SE/Player/SE_PL_Weapon_Deploy_01.wav",
    "Assets/Sound/SE/Player/SE_PL_Weapon_Deploy_02.wav",
};
inline constexpr std::string_view kPlayerWeaponStowPaths[] = {
    "Assets/Sound/SE/Player/SE_PL_Weapon_Stow_01.wav",
    "Assets/Sound/SE/Player/SE_PL_Weapon_Stow_02.wav",
};

inline Bank kPlayerFootstepWalk { kPlayerFootstepWalkPaths };
inline Bank kPlayerFootstepRun  { kPlayerFootstepRunPaths };
inline Bank kPlayerDodge        { kPlayerDodgePaths };
inline Bank kPlayerDodgeEnd     { kPlayerDodgeEndPaths };
inline Bank kPlayerLanding      { kPlayerLandingPaths };
inline Bank kPlayerDamaged      { kPlayerDamagedPaths };
inline Bank kPlayerDeath        { kPlayerDeathPaths };
inline Bank kPlayerServoTurn    { kPlayerServoTurnPaths };
inline Bank kPlayerWeaponDeploy { kPlayerWeaponDeployPaths };
inline Bank kPlayerWeaponStow   { kPlayerWeaponStowPaths };


// ── Weapon ──────────────────────────────────────────────────────────────────
// 極ごとに音が分かれているのは企画書 12.2 の「赤と青を耳でも区別する」に対応する。
// ＋ = 右手 / − = 左手 なので、バッテリーの L/R は極から一意に決まる。
inline constexpr std::string_view kBeamStartPlusPaths[]  = {
    "Assets/Sound/SE/Weapon/SE_WPN_Beam_Start_Plus.wav" };
inline constexpr std::string_view kBeamStartMinusPaths[] = {
    "Assets/Sound/SE/Weapon/SE_WPN_Beam_Start_Minus.wav" };
inline constexpr std::string_view kBeamLoopPlusPaths[]   = {
    "Assets/Sound/SE/Weapon/SE_WPN_Beam_Loop_Plus.wav" };
inline constexpr std::string_view kBeamLoopMinusPaths[]  = {
    "Assets/Sound/SE/Weapon/SE_WPN_Beam_Loop_Minus.wav" };
inline constexpr std::string_view kBeamEndPlusPaths[]    = {
    "Assets/Sound/SE/Weapon/SE_WPN_Beam_End_Plus.wav" };
inline constexpr std::string_view kBeamEndMinusPaths[]   = {
    "Assets/Sound/SE/Weapon/SE_WPN_Beam_End_Minus.wav" };

inline constexpr std::string_view kPaintConfirmPlusPaths[] = {
    "Assets/Sound/SE/Weapon/SE_WPN_Paint_Confirm_Plus_01.wav",
    "Assets/Sound/SE/Weapon/SE_WPN_Paint_Confirm_Plus_02.wav",
    "Assets/Sound/SE/Weapon/SE_WPN_Paint_Confirm_Plus_03.wav",
    "Assets/Sound/SE/Weapon/SE_WPN_Paint_Confirm_Plus_04.wav",
};
inline constexpr std::string_view kPaintConfirmMinusPaths[] = {
    "Assets/Sound/SE/Weapon/SE_WPN_Paint_Confirm_Minus_01.wav",
    "Assets/Sound/SE/Weapon/SE_WPN_Paint_Confirm_Minus_02.wav",
    "Assets/Sound/SE/Weapon/SE_WPN_Paint_Confirm_Minus_03.wav",
    "Assets/Sound/SE/Weapon/SE_WPN_Paint_Confirm_Minus_04.wav",
};
inline constexpr std::string_view kPaintExtendPaths[] = {
    "Assets/Sound/SE/Weapon/SE_WPN_Paint_Extend_01.wav",
    "Assets/Sound/SE/Weapon/SE_WPN_Paint_Extend_02.wav",
};
inline constexpr std::string_view kNeutralizePaths[] = {
    "Assets/Sound/SE/Weapon/SE_WPN_Neutralize_01.wav",
    "Assets/Sound/SE/Weapon/SE_WPN_Neutralize_02.wav",
};
inline constexpr std::string_view kTapPlusPaths[] = {
    "Assets/Sound/SE/Weapon/SE_WPN_Tap_Plus_01.wav",
    "Assets/Sound/SE/Weapon/SE_WPN_Tap_Plus_02.wav",
};
inline constexpr std::string_view kTapMinusPaths[] = {
    "Assets/Sound/SE/Weapon/SE_WPN_Tap_Minus_01.wav",
    "Assets/Sound/SE/Weapon/SE_WPN_Tap_Minus_02.wav",
};
inline constexpr std::string_view kBatteryEmptyRightPaths[] = {
    "Assets/Sound/SE/Weapon/SE_WPN_Battery_Empty_R.wav" };
inline constexpr std::string_view kBatteryEmptyLeftPaths[]  = {
    "Assets/Sound/SE/Weapon/SE_WPN_Battery_Empty_L.wav" };
inline constexpr std::string_view kBatteryFullRightPaths[]  = {
    "Assets/Sound/SE/Weapon/SE_WPN_Battery_Full_R.wav" };
inline constexpr std::string_view kBatteryFullLeftPaths[]   = {
    "Assets/Sound/SE/Weapon/SE_WPN_Battery_Full_L.wav" };
inline constexpr std::string_view kWarnNeutralPaths[] = {
    "Assets/Sound/SE/Weapon/SE_WPN_Warn_Neutral.wav" };

inline Bank kBeamStartPlus     { kBeamStartPlusPaths };
inline Bank kBeamStartMinus    { kBeamStartMinusPaths };
inline Bank kBeamLoopPlus      { kBeamLoopPlusPaths };
inline Bank kBeamLoopMinus     { kBeamLoopMinusPaths };
inline Bank kBeamEndPlus       { kBeamEndPlusPaths };
inline Bank kBeamEndMinus      { kBeamEndMinusPaths };
inline Bank kPaintConfirmPlus  { kPaintConfirmPlusPaths };
inline Bank kPaintConfirmMinus { kPaintConfirmMinusPaths };
inline Bank kPaintExtend       { kPaintExtendPaths };
inline Bank kNeutralize        { kNeutralizePaths };
inline Bank kTapPlus           { kTapPlusPaths };
inline Bank kTapMinus          { kTapMinusPaths };
inline Bank kBatteryEmptyRight { kBatteryEmptyRightPaths };
inline Bank kBatteryEmptyLeft  { kBatteryEmptyLeftPaths };
inline Bank kBatteryFullRight  { kBatteryFullRightPaths };
inline Bank kBatteryFullLeft   { kBatteryFullLeftPaths };
inline Bank kWarnNeutral       { kWarnNeutralPaths };

[[nodiscard]] inline const Bank& BeamStart(Polarity p)
{ return p == Polarity::Plus ? kBeamStartPlus : kBeamStartMinus; }
[[nodiscard]] inline const Bank& BeamLoop(Polarity p)
{ return p == Polarity::Plus ? kBeamLoopPlus : kBeamLoopMinus; }
[[nodiscard]] inline const Bank& BeamEnd(Polarity p)
{ return p == Polarity::Plus ? kBeamEndPlus : kBeamEndMinus; }
[[nodiscard]] inline const Bank& PaintConfirm(Polarity p)
{ return p == Polarity::Plus ? kPaintConfirmPlus : kPaintConfirmMinus; }
[[nodiscard]] inline const Bank& Tap(Polarity p)
{ return p == Polarity::Plus ? kTapPlus : kTapMinus; }
[[nodiscard]] inline const Bank& BatteryEmpty(Polarity p)
{ return p == Polarity::Plus ? kBatteryEmptyRight : kBatteryEmptyLeft; }
[[nodiscard]] inline const Bank& BatteryFull(Polarity p)
{ return p == Polarity::Plus ? kBatteryFullRight : kBatteryFullLeft; }


// ── Impact ──────────────────────────────────────────────────────────────────
inline constexpr std::string_view kImpactLightPaths[] = {
    "Assets/Sound/SE/Impact/SE_IMP_Hit_Light_01.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Hit_Light_02.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Hit_Light_03.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Hit_Light_04.wav",
};
inline constexpr std::string_view kImpactMidPaths[] = {
    "Assets/Sound/SE/Impact/SE_IMP_Hit_Mid_01.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Hit_Mid_02.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Hit_Mid_03.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Hit_Mid_04.wav",
};
inline constexpr std::string_view kImpactHeavyPaths[] = {
    "Assets/Sound/SE/Impact/SE_IMP_Hit_Heavy_01.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Hit_Heavy_02.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Hit_Heavy_03.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Hit_Heavy_04.wav",
};
inline constexpr std::string_view kImpactWallPaths[] = {
    "Assets/Sound/SE/Impact/SE_IMP_Wall_01.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Wall_02.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Wall_03.wav",
};
inline constexpr std::string_view kImpactDebrisPaths[] = {
    "Assets/Sound/SE/Impact/SE_IMP_Debris_01.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Debris_02.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Debris_03.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Debris_04.wav",
};
inline constexpr std::string_view kImpactCoreHitPaths[] = {
    "Assets/Sound/SE/Impact/SE_IMP_Core_Hit_01.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Core_Hit_02.wav",
};
inline constexpr std::string_view kImpactFinalPaths[] = {
    "Assets/Sound/SE/Impact/SE_IMP_Final_01.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Final_02.wav",
};
inline constexpr std::string_view kAttractWindupPaths[] = {
    "Assets/Sound/SE/Impact/SE_ATR_Windup_01.wav",
    "Assets/Sound/SE/Impact/SE_ATR_Windup_02.wav",
};
inline constexpr std::string_view kAttractLaunchPaths[] = {
    "Assets/Sound/SE/Impact/SE_ATR_Launch_01.wav",
    "Assets/Sound/SE/Impact/SE_ATR_Launch_02.wav",
    "Assets/Sound/SE/Impact/SE_ATR_Launch_03.wav",
};
inline constexpr std::string_view kAttractConvergePaths[] = {
    "Assets/Sound/SE/Impact/SE_ATR_Converge_01.wav",
    "Assets/Sound/SE/Impact/SE_ATR_Converge_02.wav",
};
inline constexpr std::string_view kAttractTravelLoopPaths[] = {
    "Assets/Sound/SE/Impact/SE_ATR_Travel_Loop.wav" };

inline Bank kImpactLight       { kImpactLightPaths };
inline Bank kImpactMid         { kImpactMidPaths };
inline Bank kImpactHeavy       { kImpactHeavyPaths };
inline Bank kImpactWall        { kImpactWallPaths };
inline Bank kImpactDebris      { kImpactDebrisPaths };
inline Bank kImpactCoreHit     { kImpactCoreHitPaths };
inline Bank kImpactFinal       { kImpactFinalPaths };
inline Bank kAttractWindup     { kAttractWindupPaths };
inline Bank kAttractLaunch     { kAttractLaunchPaths };
inline Bank kAttractConverge   { kAttractConvergePaths };
inline Bank kAttractTravelLoop { kAttractTravelLoopPaths };

/// 衝突の強さから軽 / 中 / 重を選ぶ。
///
/// WHY 強さで音を変えるか: ImpactFeedbackManager はヒットストップも揺れも強さで
///     配分しているのに、音だけ一定だと「強く決まった」感触が耳から抜ける。
///     素材が 3 段で用意されているので、そのまま 3 段に割る。
[[nodiscard]] inline const Bank& ImpactByStrength(float strength01)
{
    if (strength01 < 0.34f) return kImpactLight;
    if (strength01 < 0.67f) return kImpactMid;
    return kImpactHeavy;
}


// ── Enemy ───────────────────────────────────────────────────────────────────
inline constexpr std::string_view kEnemySpawnPaths[] = {
    "Assets/Sound/SE/Enemy/SE_ENE_Spawn_01.wav",
    "Assets/Sound/SE/Enemy/SE_ENE_Spawn_02.wav",
};
inline constexpr std::string_view kEnemyFlinchPaths[] = {
    "Assets/Sound/SE/Enemy/SE_ENE_Flinch_01.wav",
    "Assets/Sound/SE/Enemy/SE_ENE_Flinch_02.wav",
    "Assets/Sound/SE/Enemy/SE_ENE_Flinch_03.wav",
};
inline constexpr std::string_view kEnemyDestroyPaths[] = {
    "Assets/Sound/SE/Enemy/SE_ENE_Destroy_01.wav",
    "Assets/Sound/SE/Enemy/SE_ENE_Destroy_02.wav",
    "Assets/Sound/SE/Enemy/SE_ENE_Destroy_03.wav",
};
inline constexpr std::string_view kEnemyCoreSpawnPaths[]  = {
    "Assets/Sound/SE/Enemy/SE_ENE_Core_Spawn.wav" };
inline constexpr std::string_view kEnemyCoreLoopPaths[]   = {
    "Assets/Sound/SE/Enemy/SE_ENE_Core_Loop.wav" };
inline constexpr std::string_view kEnemyCoreExpirePaths[] = {
    "Assets/Sound/SE/Enemy/SE_ENE_Core_Expire.wav" };

inline Bank kEnemySpawn      { kEnemySpawnPaths };
inline Bank kEnemyFlinch     { kEnemyFlinchPaths };
inline Bank kEnemyDestroy    { kEnemyDestroyPaths };
inline Bank kEnemyCoreSpawn  { kEnemyCoreSpawnPaths };
inline Bank kEnemyCoreLoop   { kEnemyCoreLoopPaths };
inline Bank kEnemyCoreExpire { kEnemyCoreExpirePaths };


// ── Mite / Serpent / Roller ────────────────────────────────────
// WHY 共通の敵バンクと分けるか:
//   Spawn / Flinch は «敵という種類» の音で、3 種のどれが鳴らしても同じでよい。
//   移動の定常音と攻撃だけは機種を聞き分ける手がかりになるので、姿が見える前に
//   «何が来ているか» が耳で決まる。撃破音も分けて、倒れたのがどれかを目で追わずに済ませる。
inline constexpr std::string_view kMiteHoverLoopPaths[] = {
    "Assets/Sound/SE/Enemy/SE_MITE_Hover_Loop.wav" };
inline constexpr std::string_view kMiteAttackPaths[] = {
    "Assets/Sound/SE/Enemy/SE_MITE_Attack_01.wav",
    "Assets/Sound/SE/Enemy/SE_MITE_Attack_02.wav",
    "Assets/Sound/SE/Enemy/SE_MITE_Attack_03.wav",
};
inline constexpr std::string_view kMiteDestroyPaths[] = {
    "Assets/Sound/SE/Enemy/SE_MITE_Destroy_01.wav",
    "Assets/Sound/SE/Enemy/SE_MITE_Destroy_02.wav",
    "Assets/Sound/SE/Enemy/SE_MITE_Destroy_03.wav",
};

inline constexpr std::string_view kSerpentCrawlLoopPaths[] = {
    "Assets/Sound/SE/Enemy/SE_SERP_Crawl_Loop.wav" };
inline constexpr std::string_view kSerpentRearPaths[] = {
    "Assets/Sound/SE/Enemy/SE_SERP_Rear_01.wav",
    "Assets/Sound/SE/Enemy/SE_SERP_Rear_02.wav",
};
inline constexpr std::string_view kSerpentBitePaths[] = {
    "Assets/Sound/SE/Enemy/SE_SERP_Bite_01.wav",
    "Assets/Sound/SE/Enemy/SE_SERP_Bite_02.wav",
    "Assets/Sound/SE/Enemy/SE_SERP_Bite_03.wav",
};
inline constexpr std::string_view kSerpentDestroyPaths[] = {
    "Assets/Sound/SE/Enemy/SE_SERP_Destroy_01.wav",
    "Assets/Sound/SE/Enemy/SE_SERP_Destroy_02.wav",
};

inline constexpr std::string_view kRollerRollLoopPaths[] = {
    "Assets/Sound/SE/Enemy/SE_ROLL_Roll_Loop.wav" };
inline constexpr std::string_view kRollerAnchorPaths[] = {
    "Assets/Sound/SE/Enemy/SE_ROLL_Anchor_01.wav",
    "Assets/Sound/SE/Enemy/SE_ROLL_Anchor_02.wav",
};
inline constexpr std::string_view kRollerChargePaths[] = {
    "Assets/Sound/SE/Enemy/SE_ROLL_Charge_01.wav",
    "Assets/Sound/SE/Enemy/SE_ROLL_Charge_02.wav",
};
inline constexpr std::string_view kRollerDestroyPaths[] = {
    "Assets/Sound/SE/Enemy/SE_ROLL_Destroy_01.wav",
    "Assets/Sound/SE/Enemy/SE_ROLL_Destroy_02.wav",
};

inline Bank kMiteHoverLoop    { kMiteHoverLoopPaths };
inline Bank kMiteAttack       { kMiteAttackPaths };
inline Bank kMiteDestroy      { kMiteDestroyPaths };
inline Bank kSerpentCrawlLoop { kSerpentCrawlLoopPaths };
inline Bank kSerpentRear      { kSerpentRearPaths };
inline Bank kSerpentBite      { kSerpentBitePaths };
inline Bank kSerpentDestroy   { kSerpentDestroyPaths };
inline Bank kRollerRollLoop   { kRollerRollLoopPaths };
inline Bank kRollerAnchor     { kRollerAnchorPaths };
inline Bank kRollerCharge     { kRollerChargePaths };
inline Bank kRollerDestroy    { kRollerDestroyPaths };


// ── Polarity ────────────────────────────────────────────────────────────────
// WHY Boss だけ «クロールの 1 周期» を丸ごと 1 ファイルで持つか:
//   4 脚が順に接地する歩容は、接地 1 回ぶんを切り出して 4 回鳴らしても再現しない
//   (常に 3 本が接地したまま体重が移る音なので、脚どうしが重なっている部分が芯になる)。
//   素材は 1 周期 = 1 ファイルで書き出してあり、01→04 を順に回すと 4 周期ぶんの
//   歩容になる。だからこの 2 束だけは Pick() ではなく Next() で引く。
//
// WHY 巡回と突進で束を分けるか:
//   歩調が違うだけでなく «常に 3 本接地» の重なり方そのものが違う。同じ束を再生速度で
//   伸縮させると、速い側で接地が均等に並んでしまい «4 本足で走っている» に聞こえない。
inline constexpr std::string_view kBossStepWalkPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Step_Walk_01.wav",
    "Assets/Sound/SE/Boss/SE_BOSS_Step_Walk_02.wav",
    "Assets/Sound/SE/Boss/SE_BOSS_Step_Walk_03.wav",
    "Assets/Sound/SE/Boss/SE_BOSS_Step_Walk_04.wav",
};
inline constexpr std::string_view kBossStepChargePaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Step_Charge_01.wav",
    "Assets/Sound/SE/Boss/SE_BOSS_Step_Charge_02.wav",
    "Assets/Sound/SE/Boss/SE_BOSS_Step_Charge_03.wav",
    "Assets/Sound/SE/Boss/SE_BOSS_Step_Charge_04.wav",
};
inline constexpr std::string_view kBossServoLoopPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Servo_Loop.wav" };

inline constexpr std::string_view kBossChargeWindupPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Charge_Windup.wav" };
inline constexpr std::string_view kBossChargeRunPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Charge_Run.wav" };
inline constexpr std::string_view kBossChargeCrashPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Charge_Crash.wav" };
inline constexpr std::string_view kBossStunLoopPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Stun_Loop.wav" };
inline constexpr std::string_view kBossStunRecoverPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Stun_Recover.wav" };

// 踏みつけは 60F の表がそのまま 6 つのファイルに割ってある。順番も間隔も
// BossAudioComponent が持つ (ここは «どのファイルか» だけを言う)。
inline constexpr std::string_view kBossStompRaisePaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Stomp_Raise_01.wav",
    "Assets/Sound/SE/Boss/SE_BOSS_Stomp_Raise_02.wav",
};
inline constexpr std::string_view kBossStompImpactPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Stomp_Impact_01.wav",
    "Assets/Sound/SE/Boss/SE_BOSS_Stomp_Impact_02.wav",
};
inline constexpr std::string_view kBossStompSettlePaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Stomp_Settle.wav" };
inline constexpr std::string_view kBossStompHoldPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Stomp_Hold.wav" };
inline constexpr std::string_view kBossStompPullPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Stomp_Pull.wav" };

inline constexpr std::string_view kBossPolSwitchWarnPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_PolSwitch_Warn.wav" };
inline constexpr std::string_view kBossPolSwitchPlusPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_PolSwitch_01.wav" };
inline constexpr std::string_view kBossPolSwitchMinusPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_PolSwitch_02.wav" };
inline constexpr std::string_view kBossMagPulsePaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_MagPulse.wav" };

inline constexpr std::string_view kBossBeamChargePaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Beam_Charge.wav" };
inline constexpr std::string_view kBossBeamLoopPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Beam_Loop.wav" };
inline constexpr std::string_view kBossBeamSweepPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Beam_Sweep.wav" };
inline constexpr std::string_view kBossBeamEndPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Beam_End.wav" };

// WHY «撃たれた» 音ではないか: 8 章の通りボスは銃では削れず、HP が減るのは
//     帯電した雑魚が装甲へ激突したときだけ。素材もそれに合わせて «何かがぶつかって
//     装甲板が鳴った» 音で作られている。銃撃音を当てると «撃てば効く» と読ませてしまう。
inline constexpr std::string_view kBossDamagedPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Damaged_01.wav",
    "Assets/Sound/SE/Boss/SE_BOSS_Damaged_02.wav",
    "Assets/Sound/SE/Boss/SE_BOSS_Damaged_03.wav",
};
inline constexpr std::string_view kBossAppearPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Appear.wav" };
inline constexpr std::string_view kBossPhaseShiftPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_PhaseShift.wav" };
inline constexpr std::string_view kBossDestroyPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Destroy.wav" };

inline Bank kBossStepWalk       { kBossStepWalkPaths };
inline Bank kBossStepCharge     { kBossStepChargePaths };
inline Bank kBossServoLoop      { kBossServoLoopPaths };
inline Bank kBossChargeWindup   { kBossChargeWindupPaths };
inline Bank kBossChargeRun      { kBossChargeRunPaths };
inline Bank kBossChargeCrash    { kBossChargeCrashPaths };
inline Bank kBossStunLoop       { kBossStunLoopPaths };
inline Bank kBossStunRecover    { kBossStunRecoverPaths };
inline Bank kBossStompRaise     { kBossStompRaisePaths };
inline Bank kBossStompImpact    { kBossStompImpactPaths };
inline Bank kBossStompSettle    { kBossStompSettlePaths };
inline Bank kBossStompHold      { kBossStompHoldPaths };
inline Bank kBossStompPull      { kBossStompPullPaths };
inline Bank kBossPolSwitchWarn  { kBossPolSwitchWarnPaths };
inline Bank kBossPolSwitchPlus  { kBossPolSwitchPlusPaths };
inline Bank kBossPolSwitchMinus { kBossPolSwitchMinusPaths };
inline Bank kBossMagPulse       { kBossMagPulsePaths };
inline Bank kBossBeamCharge     { kBossBeamChargePaths };
inline Bank kBossBeamLoop       { kBossBeamLoopPaths };
inline Bank kBossBeamSweep      { kBossBeamSweepPaths };
inline Bank kBossBeamEnd        { kBossBeamEndPaths };
inline Bank kBossDamaged        { kBossDamagedPaths };
inline Bank kBossAppear         { kBossAppearPaths };
inline Bank kBossPhaseShift     { kBossPhaseShiftPaths };
inline Bank kBossDestroy        { kBossDestroyPaths };

/// 切り替わった «先» の極で選ぶ。01 = ＋ (低く重い) / 02 = − (高く鋭い)。
[[nodiscard]] inline const Bank& BossPolSwitch(Polarity p)
{ return p == Polarity::Plus ? kBossPolSwitchPlus : kBossPolSwitchMinus; }


inline constexpr std::string_view kPolarityInfectPaths[] = {
    "Assets/Sound/SE/Polarity/SE_POL_Infect_01.wav",
    "Assets/Sound/SE/Polarity/SE_POL_Infect_02.wav",
};
inline constexpr std::string_view kPolarityExpirePaths[] = {
    "Assets/Sound/SE/Polarity/SE_POL_Expire_01.wav",
    "Assets/Sound/SE/Polarity/SE_POL_Expire_02.wav",
};
inline constexpr std::string_view kPolarityExpireWarnPaths[] = {
    "Assets/Sound/SE/Polarity/SE_POL_Expire_Warn.wav" };
inline constexpr std::string_view kPolarityChargedLoopPlusPaths[] = {
    "Assets/Sound/SE/Polarity/SE_POL_Charged_Loop_Plus.wav" };
inline constexpr std::string_view kPolarityChargedLoopMinusPaths[] = {
    "Assets/Sound/SE/Polarity/SE_POL_Charged_Loop_Minus.wav" };
inline constexpr std::string_view kPolarityLinkTrailPaths[] = {
    "Assets/Sound/SE/Polarity/SE_POL_LinkTrail.wav" };

inline Bank kPolarityInfect            { kPolarityInfectPaths };
inline Bank kPolarityExpire            { kPolarityExpirePaths };
inline Bank kPolarityExpireWarn        { kPolarityExpireWarnPaths };
inline Bank kPolarityChargedLoopPlus   { kPolarityChargedLoopPlusPaths };
inline Bank kPolarityChargedLoopMinus  { kPolarityChargedLoopMinusPaths };
inline Bank kPolarityLinkTrail         { kPolarityLinkTrailPaths };

/// 同極が弾き合う «バンッ»。毎秒のように鳴るので、軽い衝突と同じ短さで扱う。
///
/// WHY 専用の素材を持たないか: まだ録っていない。SE_POL_Repulse_* を用意したら
///     ここのパスを差し替えるだけで全箇所に効く (Docs/open-questions.md に残してある)。
///     引力側 (kAttractLaunch) を流用しないのは、押しと引きが同じ音になると
///     Docs/presentation.md が «別物にする» と決めた 2 つの手が耳で区別できなくなるため。
inline Bank kPolarityRepulse           { kImpactLightPaths };

/// 斬撃が当たった瞬間。
///
/// WHY 専用の素材を持たないか: まだ録っていない。SE_BLD_Hit_* を用意したら
///     ここのパスを差し替えるだけで全箇所に効く (Docs/open-questions.md に残してある)。
/// NOTE: 空振りの «ヒュッ» は鳴らしていない。手持ちの素材で近いものが無く、
///     別の音を当てると «何が起きたのか» を取り違える方が害が大きい。
///     斬撃で最初に欲しい音はこれ (SE_BLD_Swing_*)。
inline Bank kBladeHit                  { kImpactLightPaths };

[[nodiscard]] inline const Bank& PolarityChargedLoop(Polarity p)
{ return p == Polarity::Plus ? kPolarityChargedLoopPlus : kPolarityChargedLoopMinus; }


// ── UI ──────────────────────────────────────────────────────────────────────
inline constexpr std::string_view kUiWaveStartPaths[] = {
    "Assets/Sound/SE/UI/SE_UI_WaveStart.wav" };
inline constexpr std::string_view kUiWaveClearPaths[] = {
    "Assets/Sound/SE/UI/SE_UI_WaveClear.wav" };
inline constexpr std::string_view kUiGameClearPaths[] = {
    "Assets/Sound/SE/UI/SE_UI_GameClear.wav" };
inline constexpr std::string_view kUiGameOverPaths[]  = {
    "Assets/Sound/SE/UI/SE_UI_GameOver.wav" };
inline constexpr std::string_view kUiGateOpenPaths[]  = {
    "Assets/Sound/SE/UI/SE_UI_GateOpen.wav" };
inline constexpr std::string_view kUiResultPaths[]    = {
    "Assets/Sound/SE/UI/SE_UI_Result.wav" };
inline constexpr std::string_view kUiRetryPaths[]     = {
    "Assets/Sound/SE/UI/SE_UI_Retry.wav" };
inline constexpr std::string_view kUiComboPaths[]     = {
    "Assets/Sound/SE/UI/SE_UI_Combo.wav" };
inline constexpr std::string_view kUiComboHighPaths[] = {
    "Assets/Sound/SE/UI/SE_UI_Combo_High.wav" };
inline constexpr std::string_view kUiRankSPaths[] = { "Assets/Sound/SE/UI/SE_UI_Rank_S.wav" };
inline constexpr std::string_view kUiRankAPaths[] = { "Assets/Sound/SE/UI/SE_UI_Rank_A.wav" };
inline constexpr std::string_view kUiRankBPaths[] = { "Assets/Sound/SE/UI/SE_UI_Rank_B.wav" };
inline constexpr std::string_view kUiRankCPaths[] = { "Assets/Sound/SE/UI/SE_UI_Rank_C.wav" };

inline Bank kUiWaveStart { kUiWaveStartPaths };
inline Bank kUiWaveClear { kUiWaveClearPaths };
inline Bank kUiGameClear { kUiGameClearPaths };
inline Bank kUiGameOver  { kUiGameOverPaths };
inline Bank kUiGateOpen  { kUiGateOpenPaths };
inline Bank kUiResult    { kUiResultPaths };
inline Bank kUiRetry     { kUiRetryPaths };
inline Bank kUiCombo     { kUiComboPaths };
inline Bank kUiComboHigh { kUiComboHighPaths };
inline Bank kUiRankS     { kUiRankSPaths };
inline Bank kUiRankA     { kUiRankAPaths };
inline Bank kUiRankB     { kUiRankBPaths };
inline Bank kUiRankC     { kUiRankCPaths };

/// ランク文字 ('S' / 'A' / 'B' / 'C') から鳴らす音を選ぶ。未知の文字は C 扱い。
[[nodiscard]] inline const Bank& RankBank(char rank)
{
    switch (rank) {
    case 'S': return kUiRankS;
    case 'A': return kUiRankA;
    case 'B': return kUiRankB;
    default:  return kUiRankC;
    }
}


// ── Env ─────────────────────────────────────────────────────────────────────
inline constexpr std::string_view kEnvArenaAmbPaths[]    = {
    "Assets/Sound/SE/Env/SE_ENV_Arena_Amb.wav" };
inline constexpr std::string_view kEnvCorridorAmbPaths[] = {
    "Assets/Sound/SE/Env/SE_ENV_Corridor_Amb.wav" };
inline constexpr std::string_view kEnvHazardWarnPaths[]  = {
    "Assets/Sound/SE/Env/SE_ENV_Hazard_Warn.wav" };
inline constexpr std::string_view kEnvHazardLoopPaths[]  = {
    "Assets/Sound/SE/Env/SE_ENV_Hazard_Loop.wav" };
inline constexpr std::string_view kEnvHazardDamagePaths[] = {
    "Assets/Sound/SE/Env/SE_ENV_Hazard_Damage_01.wav",
    "Assets/Sound/SE/Env/SE_ENV_Hazard_Damage_02.wav",
};

inline Bank kEnvArenaAmb     { kEnvArenaAmbPaths };
inline Bank kEnvCorridorAmb  { kEnvCorridorAmbPaths };
inline Bank kEnvHazardWarn   { kEnvHazardWarnPaths };
inline Bank kEnvHazardLoop   { kEnvHazardLoopPaths };
inline Bank kEnvHazardDamage { kEnvHazardDamagePaths };


// ── 再生の入口 ───────────────────────────────────────────────────────────────

/// Inspector の上書きがあればそれを、無ければカタログの変奏を鳴らす。
///
/// WHY Inspector を先に見るか: 特定の 1 体だけ違う音にしたい、という調整は
///     シーン側でやりたい。カタログは「何も指定しなかったときの正解」に留める。
inline void Play(const ScriptAudioProxy& audio, const std::string& overridePath,
                 const Bank& bank, float volumeScale = 1.0f)
{
    if (!overridePath.empty()) {
        audio.PlayOneShot(overridePath, volumeScale);
        return;
    }
    const std::string_view path = bank.Pick();
    if (!path.empty()) audio.PlayOneShot(path, volumeScale);
}

inline void Play(const ScriptAudioProxy& audio, const Bank& bank, float volumeScale = 1.0f)
{
    const std::string_view path = bank.Pick();
    if (!path.empty()) audio.PlayOneShot(path, volumeScale);
}

/// 束を並び順のまま送って鳴らす。周期そのものが素材に書き込まれている束に使う。
inline void PlayNext(const ScriptAudioProxy& audio, const Bank& bank, float volumeScale = 1.0f)
{
    const std::string_view path = bank.Next();
    if (!path.empty()) audio.PlayOneShot(path, volumeScale);
}

/// 発生源が既に消えている音 (撃破・弾着) を、その場所で鳴らす。
inline void PlayAt(const ScriptAudioProxy& audio, const Bank& bank,
                   const fbzz::math::Vector3& position, float volume = 1.0f)
{
    const std::string_view path = bank.Pick();
    if (!path.empty()) audio.PlayAtPoint(path, position, volume);
}


// ── 構成の自己修復 ───────────────────────────────────────────────────────────

/// 自 GameObject に AudioSource を用意する。シーンに置いてあればそれを尊重する。
/// @param bus     出力先バス (ProjectSettings > Audio で定義済みの名前)。
/// @param spatial 0 = 2D (プレイヤー本人・UI) / 1 = 3D 減衰あり (敵・環境)。
inline AudioSourceComponent& EnsureSource(const ScriptSceneProxy& sceneProxy,
                                          std::string_view bus = "SE",
                                          float spatial = 0.0f)
{
    if (auto* existing = sceneProxy.GetComponent<AudioSourceComponent>())
        return *existing;

    auto& created = sceneProxy.GetOrAddComponent<AudioSourceComponent>();
    created.busName      = std::string(bus);
    created.spatialBlend = spatial;
    // clipPath が空でも playOnAwake が立っていると AudioSystem が毎回問い合わせる。
    created.playOnAwake  = false;
    return created;
}

/// 受聴点を用意する。カメラに 1 つあれば足りる。
///
/// WHY 必要か: Listener が 1 つも無いと AudioSystem は距離減衰の基準を持てず、
///     3D 音源が一切鳴らない。しかも警告は出ないので、症状は「敵の音だけ無音」になる。
inline void EnsureListener(const ScriptSceneProxy& sceneProxy)
{
    (void)sceneProxy.GetOrAddComponent<AudioListenerComponent>();
}

} // namespace sandbox::se
