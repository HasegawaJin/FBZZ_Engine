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
#include <Scripts/Utils/BladeColors.hpp>
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
// 脚を掴んで登るときの一手。0.83 秒周期で左右が入れ替わるので、変奏どうしの
// 音量差 (-1.8 / 0.0 / -0.9 dB) はファイルに焼き込んである。ここで揃えないこと。
inline constexpr std::string_view kPlayerClimbGrabPaths[] = {
    "Assets/Sound/SE/Player/SE_PL_Climb_Grab_01.wav",
    "Assets/Sound/SE/Player/SE_PL_Climb_Grab_02.wav",
    "Assets/Sound/SE/Player/SE_PL_Climb_Grab_03.wav",
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
inline Bank kPlayerClimbGrab    { kPlayerClimbGrabPaths };

// ── Blade (双剣) ────────────────────────────────────────────────────────────
// 極ごとに音が分かれているのは企画書 12.2 の「赤と青を耳でも区別する」に対応する。
// ＋ = 右手 / − = 左手 なので、極が決まれば手も一意に決まる。
//
// WHY 銃の音を «消した» か:
//   企画が Wave 制の銃から双剣へ移った時点で、Beam / Paint / Tap / Battery は
//   鳴らす場所そのものが無くなった。定義だけ残すと «使える音がある» ように見えて、
//   次に音を足す人が空振りする。素材ごと畳んで、双剣の音と 1 対 1 で並べ直す。
//
// WHY 段ごとに別の音を持つか:
//   1 段目と 3 段目が同じ «シュッ» だと、連撃のどこに居るかが耳で追えない。
//   3 段目 (締め) だけ長く重くしてあるので、次が繋がるかどうかが音で判る。
inline constexpr std::string_view kBladeSwingR1Paths[] = {
    "Assets/Sound/SE/Weapon/SE_BLD_Swing_R_1st_01.wav",
    "Assets/Sound/SE/Weapon/SE_BLD_Swing_R_1st_02.wav",
};
inline constexpr std::string_view kBladeSwingR2Paths[] = {
    "Assets/Sound/SE/Weapon/SE_BLD_Swing_R_2nd_01.wav",
    "Assets/Sound/SE/Weapon/SE_BLD_Swing_R_2nd_02.wav",
};
inline constexpr std::string_view kBladeSwingR3Paths[] = {
    "Assets/Sound/SE/Weapon/SE_BLD_Swing_R_3rd_01.wav",
    "Assets/Sound/SE/Weapon/SE_BLD_Swing_R_3rd_02.wav",
};
inline constexpr std::string_view kBladeSwingL1Paths[] = {
    "Assets/Sound/SE/Weapon/SE_BLD_Swing_L_1st_01.wav",
    "Assets/Sound/SE/Weapon/SE_BLD_Swing_L_1st_02.wav",
};
inline constexpr std::string_view kBladeSwingL2Paths[] = {
    "Assets/Sound/SE/Weapon/SE_BLD_Swing_L_2nd_01.wav",
    "Assets/Sound/SE/Weapon/SE_BLD_Swing_L_2nd_02.wav",
};
inline constexpr std::string_view kBladeSwingL3Paths[] = {
    "Assets/Sound/SE/Weapon/SE_BLD_Swing_L_3rd_01.wav",
    "Assets/Sound/SE/Weapon/SE_BLD_Swing_L_3rd_02.wav",
};

inline constexpr std::string_view kBladeHitRPaths[] = {
    "Assets/Sound/SE/Weapon/SE_BLD_Hit_R_01.wav",
    "Assets/Sound/SE/Weapon/SE_BLD_Hit_R_02.wav",
    "Assets/Sound/SE/Weapon/SE_BLD_Hit_R_03.wav",
};
inline constexpr std::string_view kBladeHitLPaths[] = {
    "Assets/Sound/SE/Weapon/SE_BLD_Hit_L_01.wav",
    "Assets/Sound/SE/Weapon/SE_BLD_Hit_L_02.wav",
    "Assets/Sound/SE/Weapon/SE_BLD_Hit_L_03.wav",
};
inline constexpr std::string_view kBladeHitFinishRPaths[] = {
    "Assets/Sound/SE/Weapon/SE_BLD_Hit_Finish_R.wav" };
inline constexpr std::string_view kBladeHitFinishLPaths[] = {
    "Assets/Sound/SE/Weapon/SE_BLD_Hit_Finish_L.wav" };

inline constexpr std::string_view kBladeChargePlusPaths[]  = {
    "Assets/Sound/SE/Weapon/SE_BLD_Charge_Plus.wav" };
inline constexpr std::string_view kBladeChargeMinusPaths[] = {
    "Assets/Sound/SE/Weapon/SE_BLD_Charge_Minus.wav" };
// 溜めの持続音。振幅変調は PlayerTuning の chargeShakeHz (24Hz) に合わせてあるので、
// 画面の震えと音のうねりが同じ周期で来る。別々の数字にすると «ずれている» と感じる。
inline constexpr std::string_view kBladeChargeUpLoopPaths[] = {
    "Assets/Sound/SE/Weapon/SE_BLD_ChargeUp_Loop.wav" };
inline constexpr std::string_view kBladeChargeUpFullPaths[] = {
    "Assets/Sound/SE/Weapon/SE_BLD_ChargeUp_Full.wav" };
inline constexpr std::string_view kBladeChargeSlashRPaths[] = {
    "Assets/Sound/SE/Weapon/SE_BLD_ChargeSlash_R.wav" };
inline constexpr std::string_view kBladeChargeSlashLPaths[] = {
    "Assets/Sound/SE/Weapon/SE_BLD_ChargeSlash_L.wav" };

inline constexpr std::string_view kBladeDashPaths[] = {
    "Assets/Sound/SE/Weapon/SE_BLD_Dash_01.wav",
    "Assets/Sound/SE/Weapon/SE_BLD_Dash_02.wav",
};
// ＋と−が打ち消し合って無極へ落ちる音。斬撃そのものではなく «帯びが消えた» 合図。
inline constexpr std::string_view kNeutralizePaths[] = {
    "Assets/Sound/SE/Weapon/SE_BLD_Neutralize_01.wav",
    "Assets/Sound/SE/Weapon/SE_BLD_Neutralize_02.wav",
};

inline Bank kBladeSwingR1     { kBladeSwingR1Paths };
inline Bank kBladeSwingR2     { kBladeSwingR2Paths };
inline Bank kBladeSwingR3     { kBladeSwingR3Paths };
inline Bank kBladeSwingL1     { kBladeSwingL1Paths };
inline Bank kBladeSwingL2     { kBladeSwingL2Paths };
inline Bank kBladeSwingL3     { kBladeSwingL3Paths };
inline Bank kBladeHitR        { kBladeHitRPaths };
inline Bank kBladeHitL        { kBladeHitLPaths };
inline Bank kBladeHitFinishR  { kBladeHitFinishRPaths };
inline Bank kBladeHitFinishL  { kBladeHitFinishLPaths };
inline Bank kBladeChargePlus  { kBladeChargePlusPaths };
inline Bank kBladeChargeMinus { kBladeChargeMinusPaths };
inline Bank kBladeChargeUpLoop { kBladeChargeUpLoopPaths };
inline Bank kBladeChargeUpFull { kBladeChargeUpFullPaths };
inline Bank kBladeChargeSlashR { kBladeChargeSlashRPaths };
inline Bank kBladeChargeSlashL { kBladeChargeSlashLPaths };
inline Bank kBladeDash        { kBladeDashPaths };
inline Bank kNeutralize       { kNeutralizePaths };

/// 空振りの «ヒュッ»。stage は 0 = 入口 / 1 = 途中 / 2 = 締め の 3 段階。
///
/// WHY 段番号そのものを受け取らないか: 素材は 3 種類しか無いのに連撃は 5 段ある。
///     ここで «4 段目は何の音か» を決めると、段構成を変えるたびに音の割り当てが
///     置き去りになる。«連撃のどこか» へ畳むのは段を数えている呼び出し側の仕事。
[[nodiscard]] inline const Bank& BladeSwing(BladeSide p, int stage)
{
    const int s = stage < 0 ? 0 : (stage > 2 ? 2 : stage);
    if (p == BladeSide::Right)
        return s == 0 ? kBladeSwingR1 : s == 1 ? kBladeSwingR2 : kBladeSwingR3;
    return s == 0 ? kBladeSwingL1 : s == 1 ? kBladeSwingL2 : kBladeSwingL3;
}
[[nodiscard]] inline const Bank& BladeHit(BladeSide p)
{ return p == BladeSide::Right ? kBladeHitR : kBladeHitL; }
[[nodiscard]] inline const Bank& BladeHitFinish(BladeSide p)
{ return p == BladeSide::Right ? kBladeHitFinishR : kBladeHitFinishL; }
[[nodiscard]] inline const Bank& BladeCharge(BladeSide p)
{ return p == BladeSide::Right ? kBladeChargePlus : kBladeChargeMinus; }
[[nodiscard]] inline const Bank& BladeChargeSlash(BladeSide p)
{ return p == BladeSide::Right ? kBladeChargeSlashR : kBladeChargeSlashL; }

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
// 押し込み撃破。ハザードや壁へ «押して» 落としたときだけ鳴る。
// WHY 通常の重い当たりと分けるか: 押し込みはランクの 2 軸の片方 (押した側) で、
//     斬って倒したのと別に数えている。耳でも別でないと «今のは押しで入った» が
//     分からず、評価だけが勝手に動いているように見える。
inline constexpr std::string_view kImpactPushKillPaths[] = {
    "Assets/Sound/SE/Impact/SE_IMP_PushKill_01.wav",
    "Assets/Sound/SE/Impact/SE_IMP_PushKill_02.wav",
    "Assets/Sound/SE/Impact/SE_IMP_PushKill_03.wav",
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
inline Bank kImpactPushKill    { kImpactPushKillPaths };
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

// ── BladeSide ────────────────────────────────────────────────────────────────
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
// 背のコアを覆う蓋。開いている間だけコアが斬れるので、**開と閉は状態の合図**であって
// 演出ではない。閉じる音を «開く音の逆再生» にしないのはそのため ─ 閉じは掛け金が
// 噛む 1 発で終わらせて «窓が閉じた» と分かるようにしてある。
inline constexpr std::string_view kBossHatchOpenPaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Hatch_Open.wav",
};
inline constexpr std::string_view kBossHatchClosePaths[] = {
    "Assets/Sound/SE/Boss/SE_BOSS_Hatch_Close.wav",
};
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
inline Bank kBossMagPulse       { kBossMagPulsePaths };
inline Bank kBossBeamCharge     { kBossBeamChargePaths };
inline Bank kBossBeamLoop       { kBossBeamLoopPaths };
inline Bank kBossBeamSweep      { kBossBeamSweepPaths };
inline Bank kBossBeamEnd        { kBossBeamEndPaths };
inline Bank kBossDamaged        { kBossDamagedPaths };
inline Bank kBossAppear         { kBossAppearPaths };
inline Bank kBossPhaseShift     { kBossPhaseShiftPaths };
inline Bank kBossDestroy        { kBossDestroyPaths };
inline Bank kBossHatchOpen      { kBossHatchOpenPaths };
inline Bank kBossHatchClose     { kBossHatchClosePaths };

/// 刃が弾かれる «バンッ»。毎秒のように鳴るので、軽い衝突と同じ短さで扱う。
///
/// WHY 残響を掛けていないか: Docs/presentation.md が «画面に残らないこと» を要件に
///     挙げている。アリーナの反射を足すと 1 発ごとに 230ms 尾を引いて、連続で
///     弾いたときに «まだ前のが鳴っている» 状態になる。
inline constexpr std::string_view kBladeRepulsePaths[] = {
    "Assets/Sound/SE/Impact/SE_IMP_Repulse_01.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Repulse_02.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Repulse_03.wav",
    "Assets/Sound/SE/Impact/SE_IMP_Repulse_04.wav",
};
inline Bank kBladeRepulse           { kBladeRepulsePaths };

// ── UI ──────────────────────────────────────────────────────────────────────
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
inline constexpr std::string_view kUiLockOnPaths[]     = {
    "Assets/Sound/SE/UI/SE_UI_Lock_On.wav" };
inline constexpr std::string_view kUiLockSwitchPaths[] = {
    "Assets/Sound/SE/UI/SE_UI_Lock_Switch.wav" };
inline constexpr std::string_view kUiRankSPaths[] = { "Assets/Sound/SE/UI/SE_UI_Rank_S.wav" };
inline constexpr std::string_view kUiRankAPaths[] = { "Assets/Sound/SE/UI/SE_UI_Rank_A.wav" };
inline constexpr std::string_view kUiRankBPaths[] = { "Assets/Sound/SE/UI/SE_UI_Rank_B.wav" };
inline constexpr std::string_view kUiRankCPaths[] = { "Assets/Sound/SE/UI/SE_UI_Rank_C.wav" };

// NOTE: Wave 制は企画から外れた。WaveDirectorComponent と SE_UI_Wave* も削除済み。
//       «ステージが始まる» の合図は kUiGateOpen (ボス部屋の扉が開く音) が受け持つ。
inline Bank kUiGameClear { kUiGameClearPaths };
inline Bank kUiGameOver  { kUiGameOverPaths };
inline Bank kUiGateOpen  { kUiGateOpenPaths };
inline Bank kUiResult    { kUiResultPaths };
inline Bank kUiRetry     { kUiRetryPaths };
inline Bank kUiCombo     { kUiComboPaths };
inline Bank kUiComboHigh { kUiComboHighPaths };
inline Bank kUiLockOn     { kUiLockOnPaths };
inline Bank kUiLockSwitch { kUiLockSwitchPaths };
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
