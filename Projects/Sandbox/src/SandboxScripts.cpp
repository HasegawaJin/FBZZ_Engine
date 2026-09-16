/// @file    SandboxScripts.cpp
/// @brief   Sandbox 固有 Script の ScriptFactory 登録 (EXE 側静的リンク)。
/// @author  Hasegawa Jin
/// @date    2026-06-02
///
/// WHY: DLL ホットリロードが有効な場合、これらの登録は ScriptDllLoader が上書きする。
/// DLL 未ビルド時の起動フォールバック、および RuntimeBuild した Standalone exe の
/// スクリプト登録として機能する。
/// WHY (マーカーコメント方式):
/// ScriptCodeGen が Assets/**/*.hpp の FBZZ_SCRIPT(...) をスキャンし、
/// @@FBZZ_SCRIPT_INCLUDES_BEGIN/END の間を自動同期する。エントリは Scripts/ScriptList.inl で一元管理する。

// @@FBZZ_SCRIPT_INCLUDES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
#include "Scripts/PlayerControllerComponent.hpp"
#include "Scripts/SceneManagerScript.hpp"
#include "Scripts/TpsCameraComponent.hpp"
// @@FBZZ_SCRIPT_INCLUDES_END

// WHY: ScriptSceneProxy::GetComponent<T>() のテンプレート定義は Scene.hpp 末尾にある。
//      スクリプトヘッダは Script.hpp しかインクルードしないため、
//      全 GetComponent 特殊化をこの TU でインスタンス化するために明示インクルードが必要。
//      SandboxScriptsDll.cpp でも同じ理由で Scene.hpp をインクルードしている。
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptFactory.hpp>

// WHY: エントリは Scripts/ScriptList.inl で一元管理する。
//      ScriptCodeGen は ScriptList.inl だけを更新するため、このファイルのエントリを手動編集する必要はない。
//      FBZZ_SCRIPT_ENTRY(ns, T) → FBZZ_REGISTER_SCRIPT(::ns::T) に展開される。
#define FBZZ_SCRIPT_ENTRY(ns, T) FBZZ_REGISTER_SCRIPT(::ns::T)
#include "Scripts/ScriptList.inl"
#undef FBZZ_SCRIPT_ENTRY
