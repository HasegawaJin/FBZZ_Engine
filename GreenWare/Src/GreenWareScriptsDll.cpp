// GreenWare
// GreenWareScriptsDll.cpp | greenware
// スクリプト DLL のエントリポイント
//
// WHY (コールバック渡し設計):
//   fbzz_engine は shared runtime として EXE / Script DLL から共有される。
//   ただし Script DLL は任意のユーザーコードを後からロードする拡張境界なので、
//   登録 API は DLL 側からグローバル状態へ暗黙アクセスするより、EXE が渡す関数ポインタ経由にする。
//   これにより ScriptFactory の所有者をホスト側へ固定し、将来の外部プラグイン SDK 化でも
//   境界が明確なまま保てる。
//
// スクリプト追加手順:
//   1. Assets/Scripts/ に Xxx.hpp を作成 (Script 継承、TYPE_NAME 定義)
//   2. @@FBZZ_SCRIPT_INCLUDES_BEGIN/END の include を同期
//   3. Assets/Scripts/ScriptList.inl の FBZZ_SCRIPT_ENTRY(ns, Xxx) を同期
//   → Editor の AssetBrowser から "Create → C++ Script..." でも自動生成できる

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptDllAbi.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

// DataAsset 型を DataAssetFactory へ直接自己登録する (純共有 ScriptableObject)。
#define FBZZ_DATA_ASSET_ENTRY(ns, T) FBZZ_REGISTER_DATA_ASSET(::ns::T)
#include "Scripts/DataAssetList.inl"
#undef FBZZ_DATA_ASSET_ENTRY

#ifdef GAMESCRIPTS_EXPORTS
#  define GAMESCRIPTS_API __declspec(dllexport)
#else
#  define GAMESCRIPTS_API __declspec(dllimport)
#endif

namespace {

struct ScriptEntry {
    std::string name;
    std::function<std::unique_ptr<fbzz::scene::Script>()> factory;
};

// WHY: エントリは Assets/Scripts/ScriptList.inl で一元管理する。
//      ScriptCodeGen は ScriptList.inl と include ブロックを同期するため、このファイルのエントリは手動編集不要。
const std::vector<ScriptEntry>& AllEntries()
{
    static const std::vector<ScriptEntry> entries = {
#define FBZZ_SCRIPT_ENTRY(ns, T) \
        { ::ns::T::TYPE_NAME, []() { return std::make_unique<::ns::T>(); } },
#include "Scripts/ScriptList.inl"
#undef FBZZ_SCRIPT_ENTRY
    };
    return entries;
}

} // namespace

extern "C" {

// ホストと DLL の型レイアウトが一致する場合だけ ScriptFactory 登録を許可する。
// WHY: 個別フィールドを返すことで ValidateAbi() がミスマッチ箇所をログに出力できる。
GAMESCRIPTS_API fbzz::scene::ScriptDllAbiInfo FBZZScripts_GetAbiInfo()
{
    return fbzz::scene::GetScriptDllAbiInfo();
}

GAMESCRIPTS_API int FBZZScripts_Count()
{
    return static_cast<int>(AllEntries().size());
}

GAMESCRIPTS_API const char* FBZZScripts_TypeName(int i)
{
    const auto& entries = AllEntries();
    if (i < 0 || i >= static_cast<int>(entries.size())) return "";
    return entries[static_cast<size_t>(i)].name.c_str();
}

// WHY: FBZZScripts_Register はエンジン共通のエントリポイント名。
//      ScriptDllLoader はこの名前だけを探すため、プロジェクト固有名を使わない。
GAMESCRIPTS_API void FBZZScripts_Register(
    void(*registerFn)(const char* typeName, std::function<std::unique_ptr<fbzz::scene::Script>()>))
{
    if (!registerFn) return;
    for (const auto& entry : AllEntries())
        registerFn(entry.name.c_str(), entry.factory);
}

} // extern "C"
