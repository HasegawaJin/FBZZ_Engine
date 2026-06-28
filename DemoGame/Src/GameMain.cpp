// DemoGame
// GameMain.cpp | demogame
// スクリプト静的登録 (RuntimeBuild Standalone exe 用)
//
// WHAT: RegisterScripts() は editor / standalone どちらの起動でも呼ばれる。
//       ScriptCodeGen が新規スクリプト生成時に @@FBZZ_SCRIPT_INCLUDES_BEGIN/END の間に
//       #include を自動追記する。エントリポイント (main) は AppMain.cpp にある。
//
// WHY (EXE / DLL エントリ一元管理):
//   エディタのホットリロードは DemoGameScripts.dll をロードして ScriptFactory を更新する。
//   RuntimeBuild Standalone exe は DLL をロードせず、FBZZ_REGISTER_SCRIPT で解決する。
//   登録エントリは Assets/Scripts/ScriptList.inl (X-macro ファイル) で一元管理されるため、
//   ScriptCodeGen は ScriptList.inl のみを更新すればよく、このファイルのエントリは手動編集不要。
#include "DemoGame/ProjectAPI.hpp"
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Scene.hpp>

// @@FBZZ_SCRIPT_INCLUDES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
#include "Scripts/EnemyControllerComponent.hpp"
#include "Scripts/PlayerControllerComponent.hpp"
#include "Scripts/PlayerIKComponent.hpp"
#include "Scripts/TpsCameraComponent.hpp"
#include "Scripts/SceneManagerScript.hpp"
#include "Scripts/SwordParticleComponent.hpp"
#include "Scripts/HitBloodEffectComponent.hpp"
#include "Scripts/SwordTrailComponent.hpp"
#include "Scripts/WeaponHitboxComponent.hpp"
// @@FBZZ_SCRIPT_INCLUDES_END

// WHY: エントリは Assets/Scripts/ScriptList.inl で一元管理する。
//      ScriptCodeGen は ScriptList.inl だけを更新するため、このファイルのエントリを手動編集する必要はない。
//      FBZZ_SCRIPT_ENTRY(ns, T) → FBZZ_REGISTER_SCRIPT(::ns::T) に展開される。
#define FBZZ_SCRIPT_ENTRY(ns, T) FBZZ_REGISTER_SCRIPT(::ns::T)
#include "Scripts/ScriptList.inl"
#undef FBZZ_SCRIPT_ENTRY

namespace demogame {

void RegisterScripts()
{
    // WHY: 登録自体は上の静的初期化で完了する。
    //      AppMain.cpp から呼ばれる明示的な初期化 API を残し、起動手順の意図を保つ。
}

} // namespace demogame
