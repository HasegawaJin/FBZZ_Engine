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
#include "Scripts/Camera/TpsCameraComponent.hpp"
#include "Scripts/Combat/EnemyChaserComponent.hpp"
#include "Scripts/Combat/EnemyHealthBarComponent.hpp"
#include "Scripts/Combat/EnemyHealthComponent.hpp"
#include "Scripts/Data/PlayerTuning.hpp"
#include "Scripts/Data/PolarityTuning.hpp"
#include "Scripts/Game/CameraFollowManagerComponent.hpp"
#include "Scripts/Game/CameraShakeManagerComponent.hpp"
#include "Scripts/Game/CombatManagerComponent.hpp"
#include "Scripts/Game/GameFlowComponent.hpp"
#include "Scripts/Game/HitstopManagerComponent.hpp"
#include "Scripts/Game/ImpactFeedbackManagerComponent.hpp"
#include "Scripts/Game/ResultPresenterComponent.hpp"
#include "Scripts/Game/RumbleManagerComponent.hpp"
#include "Scripts/Game/ScreenEffectManagerComponent.hpp"
#include "Scripts/Game/TimeManagerComponent.hpp"
#include "Scripts/Player/AimMarkerComponent.hpp"
#include "Scripts/Player/BeamScorchComponent.hpp"
#include "Scripts/Player/CrosshairComponent.hpp"
#include "Scripts/Player/EyeBlinkComponent.hpp"
#include "Scripts/Player/PlayerAimComponent.hpp"
#include "Scripts/Player/PlayerComponent.hpp"
#include "Scripts/Player/PlayerControllerComponent.hpp"
#include "Scripts/Player/PlayerHeadLookComponent.hpp"
#include "Scripts/Player/PlayerHealthBarComponent.hpp"
#include "Scripts/Player/PlayerHealthComponent.hpp"
#include "Scripts/Player/PolarityGunComponent.hpp"
#include "Scripts/Player/PolarityGunHudComponent.hpp"
#include "Scripts/Player/WeaponAnimatorComponent.hpp"
#include "Scripts/Player/WeaponRigComponent.hpp"
#include "Scripts/Polarity/PolarityBodyComponent.hpp"
#include "Scripts/Polarity/PolarityFieldComponent.hpp"
#include "Scripts/Polarity/PolarityTargetComponent.hpp"
#include "Scripts/SceneManagerScript.hpp"
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
