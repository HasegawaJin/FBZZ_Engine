// GreenWare
// GameMain.cpp | greenware
// スクリプト静的登録 (RuntimeBuild Standalone exe 用)
//
// WHAT: RegisterScripts() は editor / standalone どちらの起動でも呼ばれる。
//       ScriptCodeGen が Assets/**/*.hpp の FBZZ_SCRIPT(...) をスキャンし、
//       @@FBZZ_SCRIPT_INCLUDES_BEGIN/END の間を自動同期する。
//       エントリポイント (main) は AppMain.cpp にある — ここには書かないこと。
//
// WHY (DLL と静的の二重登録):
//   エディタのホットリロードは GreenWareScripts.dll をロードして ScriptFactory を更新する。
//   RuntimeBuild Standalone exe は DLL をロードせず、ここの FBZZ_REGISTER_SCRIPT で解決する。
//   ScriptCodeGen は ScriptList.inl と include ブロックを同期するため、登録の手動編集は不要。
#include "GreenWare/ProjectAPI.hpp"
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Scene/Scene.hpp>

// @@FBZZ_SCRIPT_INCLUDES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
#include "Scripts/Camera/BossCameraDirectorComponent.hpp"
#include "Scripts/Camera/FpsCameraComponent.hpp"
#include "Scripts/Camera/TpsCameraComponent.hpp"
#include "Scripts/Combat/BossAiComponent.hpp"
#include "Scripts/Combat/BossAnimatorComponent.hpp"
#include "Scripts/Combat/BossAudioComponent.hpp"
#include "Scripts/Combat/BossBeamComponent.hpp"
#include "Scripts/Combat/BossBreakComponent.hpp"
#include "Scripts/Combat/BossCollapsePostureComponent.hpp"
#include "Scripts/Combat/BossCoreComponent.hpp"
#include "Scripts/Combat/BossDeathVfxComponent.hpp"
#include "Scripts/Combat/BossHatchComponent.hpp"
#include "Scripts/Combat/BossHealthBarComponent.hpp"
#include "Scripts/Combat/BossHitboxRigComponent.hpp"
#include "Scripts/Combat/BossLegHealthBarComponent.hpp"
#include "Scripts/Combat/BossPartComponent.hpp"
#include "Scripts/Combat/BossPartDebrisComponent.hpp"
#include "Scripts/Combat/BossPartTelegraphComponent.hpp"
#include "Scripts/Combat/BossRigComponent.hpp"
#include "Scripts/Combat/BossRoomTriggerComponent.hpp"
#include "Scripts/Combat/BossShockwaveComponent.hpp"
#include "Scripts/Combat/BossStepDustComponent.hpp"
#include "Scripts/Combat/BossTelegraphComponent.hpp"
#include "Scripts/Combat/DangerWallComponent.hpp"
#include "Scripts/Combat/EnemyDeathVfxComponent.hpp"
#include "Scripts/Combat/EnemyHealthComponent.hpp"
#include "Scripts/Combat/EyeSpriteComponent.hpp"
#include "Scripts/Combat/IBoss.hpp"
#include "Scripts/Combat/IDamageable.hpp"
#include "Scripts/Combat/LaserVolleyComponent.hpp"
#include "Scripts/Combat/LoomAiComponent.hpp"
#include "Scripts/Combat/LoomBossComponent.hpp"
#include "Scripts/Combat/SerpentAiComponent.hpp"
#include "Scripts/Combat/SerpentApertureComponent.hpp"
#include "Scripts/Combat/SerpentBodyComponent.hpp"
#include "Scripts/Combat/SerpentBossComponent.hpp"
#include "Scripts/Combat/SerpentDeathVfxComponent.hpp"
#include "Scripts/Combat/SerpentHitboxRigComponent.hpp"
#include "Scripts/Combat/SerpentPathComponent.hpp"
#include "Scripts/Combat/SerpentRigComponent.hpp"
#include "Scripts/Combat/SerpentSpineComponent.hpp"
#include "Scripts/Data/BladeTuning.hpp"
#include "Scripts/Data/PlayerTuning.hpp"
#include "Scripts/Game/ArenaBoundsComponent.hpp"
#include "Scripts/Game/ArenaHazardComponent.hpp"
#include "Scripts/Game/CameraFollowManagerComponent.hpp"
#include "Scripts/Game/CameraShakeManagerComponent.hpp"
#include "Scripts/Game/ChainDisplayComponent.hpp"
#include "Scripts/Game/CombatManagerComponent.hpp"
#include "Scripts/Game/GameFlowComponent.hpp"
#include "Scripts/Game/GameSettingsComponent.hpp"
#include "Scripts/Game/HitstopManagerComponent.hpp"
#include "Scripts/Game/ImpactFeedbackManagerComponent.hpp"
#include "Scripts/Game/ResultFieldGridComponent.hpp"
#include "Scripts/Game/ResultPresenterComponent.hpp"
#include "Scripts/Game/RumbleManagerComponent.hpp"
#include "Scripts/Game/ScreenEffectManagerComponent.hpp"
#include "Scripts/Game/TimeManagerComponent.hpp"
#include "Scripts/Game/VfxManagerComponent.hpp"
#include "Scripts/Player/AimMarkerComponent.hpp"
#include "Scripts/Player/BladeComponent.hpp"
#include "Scripts/Player/PlayerAimComponent.hpp"
#include "Scripts/Player/PlayerBossBlockComponent.hpp"
#include "Scripts/Player/PlayerClimbComponent.hpp"
#include "Scripts/Player/PlayerComponent.hpp"
#include "Scripts/Player/PlayerControllerComponent.hpp"
#include "Scripts/Player/PlayerHeadLookComponent.hpp"
#include "Scripts/Player/PlayerHealthBarComponent.hpp"
#include "Scripts/Player/PlayerHealthComponent.hpp"
#include "Scripts/Player/PlayerParryComponent.hpp"
#include "Scripts/Player/WeaponRigComponent.hpp"
#include "Scripts/SceneManagerScript.hpp"
#include "Scripts/Title/ControlsGuideComponent.hpp"
#include "Scripts/Title/ElectricMinusParticleComponent.hpp"
#include "Scripts/Title/ElectricPlusParticleComponent.hpp"
#include "Scripts/Title/OptionsScreenComponent.hpp"
#include "Scripts/Title/ScreenDressingComponent.hpp"
#include "Scripts/Title/TitleFieldGridComponent.hpp"
#include "Scripts/Title/TitleMenuComponent.hpp"
#include "Scripts/UI/ClimbPromptComponent.hpp"
#include "Scripts/UI/StageSelectComponent.hpp"
#include "Scripts/UI/UiHeroTextComponent.hpp"
#include "Scripts/UI/UiHintBarComponent.hpp"
#include "Scripts/UI/UiRevealComponent.hpp"
#include "Scripts/Utils/BeamTrailRendererComponent.hpp"
#include "Scripts/Utils/GameCursorComponent.hpp"
#include "Scripts/Utils/GlowPartComponent.hpp"
#include "Scripts/Utils/SeWarmupComponent.hpp"
#include "Scripts/Vfx/BeamScorchVfxComponent.hpp"
#include "Scripts/Vfx/BladeChargeGlowComponent.hpp"
#include "Scripts/Vfx/BladeSteelComponent.hpp"
#include "Scripts/Vfx/BladeTrailComponent.hpp"
#include "Scripts/Vfx/DodgeAfterimageComponent.hpp"
#include "Scripts/Vfx/ExecuteVfxComponent.hpp"
#include "Scripts/Vfx/ImpactVfxComponent.hpp"
#include "Scripts/Vfx/ParryVfxComponent.hpp"
#include "Scripts/Vfx/PartDamageHudComponent.hpp"
#include "Scripts/Vfx/RunDustVfxComponent.hpp"
#include "Scripts/Vfx/SerpentBiteVfxComponent.hpp"
#include "Scripts/Vfx/SerpentGeyserVfxComponent.hpp"
#include "Scripts/Vfx/SerpentRushVfxComponent.hpp"
#include "Scripts/Vfx/SerpentSlamVfxComponent.hpp"
#include "Scripts/Vfx/SerpentSnapVfxComponent.hpp"
#include "Scripts/Vfx/SlashCutFxComponent.hpp"
#include "Scripts/Vfx/SlashHitVfxComponent.hpp"
#include "Scripts/Vfx/SlashScarComponent.hpp"
#include "Scripts/Vfx/SpinSlashFxComponent.hpp"
#include "Scripts/Vfx/ToppleVfxComponent.hpp"
// @@FBZZ_SCRIPT_INCLUDES_END

#define FBZZ_SCRIPT_ENTRY(ns, T) FBZZ_REGISTER_SCRIPT(::ns::T)
#include "Scripts/ScriptList.inl"
#undef FBZZ_SCRIPT_ENTRY

// DataAsset 型を DataAssetFactory へ静的登録する (純共有 ScriptableObject)。
#define FBZZ_DATA_ASSET_ENTRY(ns, T) FBZZ_REGISTER_DATA_ASSET(::ns::T)
#include "Scripts/DataAssetList.inl"
#undef FBZZ_DATA_ASSET_ENTRY

namespace greenware {

void RegisterScripts()
{
    // WHY: 登録自体は上の静的初期化で完了する。
    //      AppMain.cpp から呼ばれる明示的な初期化 API を残し、起動手順の意図を保つ。
}

} // namespace greenware
