// {{PROJECT_NAME}}
// GameMain.cpp | {{CPP_NAMESPACE}}
// スクリプト静的登録 (RuntimeBuild Standalone exe 用)
//
// WHAT: RegisterScripts() は editor / standalone どちらの起動でも呼ばれる。
//       ScriptCodeGen が新規スクリプト生成時に @@FBZZ_SCRIPT_INCLUDES_BEGIN/END と
//       @@FBZZ_SCRIPT_ENTRIES_BEGIN/END の間に自動追記する。
//       エントリポイント (main) は AppMain.cpp にある — ここには書かないこと。
//
// WHY (DLL と静的の二重登録):
//   エディタのホットリロードは {{TARGET_NAME}}Scripts.dll をロードして ScriptFactory を更新する。
//   RuntimeBuild Standalone exe は DLL をロードせず、ここの FBZZ_REGISTER_SCRIPT で解決する。
//   ScriptCodeGen は両方のファイルを自動更新するため、手動同期は不要。
#include "{{TARGET_NAME}}/ProjectAPI.hpp"
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Scene.hpp>

// @@FBZZ_SCRIPT_INCLUDES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
#include "Scripts/PlayerControllerComponent.hpp"
#include "Scripts/TpsCameraComponent.hpp"
// @@FBZZ_SCRIPT_INCLUDES_END

// @@FBZZ_SCRIPT_ENTRIES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
FBZZ_REGISTER_SCRIPT(::sandbox::PlayerControllerComponent)
FBZZ_REGISTER_SCRIPT(::sandbox::TpsCameraComponent)
// @@FBZZ_SCRIPT_ENTRIES_END

namespace {{CPP_NAMESPACE}} {

void RegisterScripts()
{
    // WHY: 登録自体は上の静的初期化で完了する。
    //      AppMain.cpp から呼ばれる明示的な初期化 API を残し、起動手順の意図を保つ。
}

} // namespace {{CPP_NAMESPACE}}
