// GreenWare
// ScriptList.inl — スクリプト登録 X-macro リスト
// ScriptCodeGen が自動更新する。手動編集しないこと。
//   EXE 側 (GameMain.cpp):
//     #define FBZZ_SCRIPT_ENTRY(ns, T) FBZZ_REGISTER_SCRIPT(::ns::T)
//     #include "Scripts/ScriptList.inl"
//     #undef FBZZ_SCRIPT_ENTRY
//   DLL 側 (GreenWareScriptsDll.cpp):
//     #define FBZZ_SCRIPT_ENTRY(ns, T) \
//         { ::ns::T::TYPE_NAME, []() { ret std::make_unique<::ns::T>(); } },
//     #include "Scripts/ScriptList.inl"
//     #undef FBZZ_SCRIPT_ENTRY
// ScriptCodeGen は Assets/**/*.hpp の FBZZ_SCRIPT(...) をスキャンして、この範囲を自動同期する。

// @@FBZZ_SCRIPT_ENTRIES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
FBZZ_SCRIPT_ENTRY(sandbox, TpsCameraComponent)
FBZZ_SCRIPT_ENTRY(sandbox, EnemyChaserComponent)
FBZZ_SCRIPT_ENTRY(sandbox, EnemyHealthComponent)
FBZZ_SCRIPT_ENTRY(sandbox, GameFlowComponent)
FBZZ_SCRIPT_ENTRY(sandbox, ResultPresenterComponent)
FBZZ_SCRIPT_ENTRY(sandbox, EyeBlinkComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PlayerAimComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PlayerControllerComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PlayerHealthComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PolarityGunComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PolarityBodyComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PolarityFieldComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PolarityTargetComponent)
FBZZ_SCRIPT_ENTRY(sandbox, SceneManagerScript)
// @@FBZZ_SCRIPT_ENTRIES_END
