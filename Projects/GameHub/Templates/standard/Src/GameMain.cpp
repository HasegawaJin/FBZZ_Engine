// {{PROJECT_NAME}}
// GameMain.cpp | {{CPP_NAMESPACE}}
// スクリプト静的登録 (RuntimeBuild Standalone exe 用)
//
// WHAT: RegisterScripts() は editor / standalone どちらの起動でも呼ばれる。
//       ScriptCodeGen が Assets/**/*.hpp の FBZZ_SCRIPT(...) をスキャンし、
//       @@FBZZ_SCRIPT_INCLUDES_BEGIN/END の間を自動同期する。エントリポイント (main) は AppMain.cpp にある。
//
// WHY (EXE / DLL エントリ一元管理):
//   エディタのホットリロードは {{TARGET_NAME}}Scripts.dll をロードして ScriptFactory を更新する。
//   RuntimeBuild Standalone exe は DLL をロードせず、FBZZ_REGISTER_SCRIPT で解決する。
//   登録エントリは Assets/Scripts/ScriptList.inl (X-macro ファイル) で一元管理されるため、
//   ScriptCodeGen は ScriptList.inl と include ブロックを同期するため、登録の手動編集は不要。
#include "{{TARGET_NAME}}/ProjectAPI.hpp"
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Scene/Scene.hpp>

// @@FBZZ_SCRIPT_INCLUDES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
#include "Scripts/PlayerControllerComponent.hpp"
#include "Scripts/TpsCameraComponent.hpp"
#include "Scripts/SceneManagerScript.hpp"
// @@FBZZ_SCRIPT_INCLUDES_END

// WHY: エントリは Assets/Scripts/ScriptList.inl で一元管理する。
//      ScriptCodeGen は ScriptList.inl だけを更新するため、このファイルのエントリを手動編集する必要はない。
//      FBZZ_SCRIPT_ENTRY(ns, T) → FBZZ_REGISTER_SCRIPT(::ns::T) に展開される。
#define FBZZ_SCRIPT_ENTRY(ns, T) FBZZ_REGISTER_SCRIPT(::ns::T)
#include "Scripts/ScriptList.inl"
#undef FBZZ_SCRIPT_ENTRY

// DataAsset 型を DataAssetFactory へ静的登録する (純共有 ScriptableObject)。
#define FBZZ_DATA_ASSET_ENTRY(ns, T) FBZZ_REGISTER_DATA_ASSET(::ns::T)
#include "Scripts/DataAssetList.inl"
#undef FBZZ_DATA_ASSET_ENTRY

namespace {{CPP_NAMESPACE}} {

void RegisterScripts()
{
    // WHY: 登録自体は上の静的初期化で完了する。
    //      AppMain.cpp から呼ばれる明示的な初期化 API を残し、起動手順の意図を保つ。
}

} // namespace {{CPP_NAMESPACE}}
