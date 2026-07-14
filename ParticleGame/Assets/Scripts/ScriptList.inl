// ParticleGame
// ScriptList.inl — スクリプト登録 X-macro リスト
// ScriptCodeGen が自動更新する。手動編集しないこと。
//
// 使い方:
//   EXE 側 (GameMain.cpp):
//     #define FBZZ_SCRIPT_ENTRY(ns, T) FBZZ_REGISTER_SCRIPT(::ns::T)
//     #include "Scripts/ScriptList.inl"
//     #undef FBZZ_SCRIPT_ENTRY
//
//   DLL 側 (ParticleGameScriptsDll.cpp):
//     #define FBZZ_SCRIPT_ENTRY(ns, T) \
//         { ::ns::T::TYPE_NAME, []() { return std::make_unique<::ns::T>(); } },
//     #include "Scripts/ScriptList.inl"
//     #undef FBZZ_SCRIPT_ENTRY
//
// ScriptCodeGen は Assets/**/*.hpp の FBZZ_SCRIPT(...) をスキャンして、この範囲を自動同期する。

// @@FBZZ_SCRIPT_ENTRIES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
FBZZ_SCRIPT_ENTRY(particlegame, ParticleProjectile)
FBZZ_SCRIPT_ENTRY(particlegame, ParticleTarget)
FBZZ_SCRIPT_ENTRY(sandbox, PlayerControllerComponent)
FBZZ_SCRIPT_ENTRY(particlegame, PlayerParticleVfx)
FBZZ_SCRIPT_ENTRY(sandbox, SceneManagerScript)
FBZZ_SCRIPT_ENTRY(particlegame, SparkPickup)
FBZZ_SCRIPT_ENTRY(particlegame, SparkVacuumGame)
FBZZ_SCRIPT_ENTRY(particlegame, SparkVacuumResult)
FBZZ_SCRIPT_ENTRY(sandbox, TpsCameraComponent)
// @@FBZZ_SCRIPT_ENTRIES_END
