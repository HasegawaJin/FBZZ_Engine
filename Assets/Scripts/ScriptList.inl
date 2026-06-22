// FBZZ Engine
// ScriptList.inl — スクリプト登録 X-macro リスト
// ScriptCodeGen が自動更新する。手動編集しないこと。
//
// 使い方:
//   EXE 側 (SandboxScripts.cpp):
//     #define FBZZ_SCRIPT_ENTRY(ns, T) FBZZ_REGISTER_SCRIPT(::ns::T)
//     #include "Scripts/ScriptList.inl"
//     #undef FBZZ_SCRIPT_ENTRY
//
//   DLL 側 (SandboxScriptsDll.cpp):
//     #define FBZZ_SCRIPT_ENTRY(ns, T) \
//         { ::ns::T::TYPE_NAME, []() { return std::make_unique<::ns::T>(); } },
//     #include "Scripts/ScriptList.inl"
//     #undef FBZZ_SCRIPT_ENTRY
//
// エントリを追加するには Editor の "Create → C++ Script..." を使うこと。
// ScriptCodeGen は @@FBZZ_SCRIPT_ENTRIES_BEGIN/END マーカーを認識して自動追記する。

// @@FBZZ_SCRIPT_ENTRIES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
FBZZ_SCRIPT_ENTRY(sandbox, PlayerControllerComponent)
FBZZ_SCRIPT_ENTRY(sandbox, TpsCameraComponent)
FBZZ_SCRIPT_ENTRY(sandbox, SceneManagerScript)
// @@FBZZ_SCRIPT_ENTRIES_END
