// FBZZ Engine
// SandboxScripts.cpp | fbzz::sandbox
// Sandbox 固有 Script の ScriptFactory 登録 (EXE 側静的リンク)
//
// WHY: DLL ホットリロードが有効な場合、これらの登録は ScriptDllLoader が上書きする。
//      DLL 未ビルド時の起動フォールバック、および RuntimeBuild した Standalone exe の
//      スクリプト登録として機能する。
// WHY (マーカーコメント方式):
//      ScriptCodeGen がスクリプトを新規生成した際に @@FBZZ_SCRIPT_INCLUDES_BEGIN/END と
//      @@FBZZ_SCRIPT_ENTRIES_BEGIN/END の間に自動追記する。
//      SandboxScriptsDll.cpp と同じマーカーを使うことで生成ロジックを共通化している。

// @@FBZZ_SCRIPT_INCLUDES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
// WHY: #include "Scripts/Foo.hpp" は CMakeLists の include_directories(Assets/) により
//      Assets/Scripts/Foo.hpp に解決される。
#include "Scripts/PlayerControllerComponent.hpp"
#include "Scripts/TpsCameraComponent.hpp"
// @@FBZZ_SCRIPT_INCLUDES_END

// WHY: ScriptSceneProxy::GetComponent<T>() のテンプレート定義は Scene.hpp 末尾にある。
//      スクリプトヘッダは Script.hpp しかインクルードしないため、
//      全 GetComponent 特殊化をこの TU でインスタンス化するために明示インクルードが必要。
//      SandboxScriptsDll.cpp でも同じ理由で Scene.hpp をインクルードしている。
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptFactory.hpp>

// @@FBZZ_SCRIPT_ENTRIES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
FBZZ_REGISTER_SCRIPT(::sandbox::PlayerControllerComponent)
FBZZ_REGISTER_SCRIPT(::sandbox::TpsCameraComponent)
// @@FBZZ_SCRIPT_ENTRIES_END
