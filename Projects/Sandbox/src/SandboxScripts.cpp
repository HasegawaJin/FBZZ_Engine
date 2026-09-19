/// @file    SandboxScripts.cpp
/// @brief   Sandbox 固有 Script の ScriptFactory 登録 (EXE 側静的リンク)。
/// @author  Hasegawa Jin
/// @date    2026-06-02
///
/// @note DLL ホットリロードが有効なら ScriptDllLoader がこの登録を上書きする。DLL 未ビルド時の起動フォールバックと
///       RuntimeBuild した Standalone exe の登録として機能する。
/// @note `@@FBZZ_SCRIPT_INCLUDES_BEGIN/END` は ScriptCodeGen が FBZZ_SCRIPT(...) から自動同期する。エントリは Scripts/ScriptList.inl で一元管理する。

// @@FBZZ_SCRIPT_INCLUDES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
#include "Scripts/PlayerControllerComponent.hpp"
#include "Scripts/SceneManagerScript.hpp"
#include "Scripts/TpsCameraComponent.hpp"
// @@FBZZ_SCRIPT_INCLUDES_END

/// @note ScriptSceneProxy::GetComponent<T>() の定義は Scene.hpp 末尾にある。スクリプトヘッダーは Script.hpp しか
///       include しないため、全特殊化をこの TU でインスタンス化するには明示 include が要る (SandboxScriptsDll.cpp も同様)。
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptFactory.hpp>

/// @note エントリは Scripts/ScriptList.inl で一元管理する。ScriptCodeGen は ScriptList.inl だけを更新するため、
///       このファイルのエントリは手動編集不要。FBZZ_SCRIPT_ENTRY(ns, T) は FBZZ_REGISTER_SCRIPT(::ns::T) に展開される。
#define FBZZ_SCRIPT_ENTRY(ns, T) FBZZ_REGISTER_SCRIPT(::ns::T)
#include "Scripts/ScriptList.inl"
#undef FBZZ_SCRIPT_ENTRY
