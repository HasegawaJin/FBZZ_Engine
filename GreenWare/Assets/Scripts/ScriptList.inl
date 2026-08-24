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
FBZZ_SCRIPT_ENTRY(sandbox, EnemyHealthBarComponent)
FBZZ_SCRIPT_ENTRY(sandbox, EnemyHealthComponent)
FBZZ_SCRIPT_ENTRY(sandbox, EnemyMiteComponent)
FBZZ_SCRIPT_ENTRY(sandbox, EnemyRollerComponent)
FBZZ_SCRIPT_ENTRY(sandbox, EnemySerpentComponent)
FBZZ_SCRIPT_ENTRY(sandbox, EyeSpriteComponent)
FBZZ_SCRIPT_ENTRY(sandbox, CameraFollowManagerComponent)
FBZZ_SCRIPT_ENTRY(sandbox, CameraShakeManagerComponent)
FBZZ_SCRIPT_ENTRY(sandbox, ChainDisplayComponent)
FBZZ_SCRIPT_ENTRY(sandbox, CombatManagerComponent)
FBZZ_SCRIPT_ENTRY(sandbox, GameFlowComponent)
FBZZ_SCRIPT_ENTRY(sandbox, GameSettingsComponent)
FBZZ_SCRIPT_ENTRY(sandbox, HitstopManagerComponent)
FBZZ_SCRIPT_ENTRY(sandbox, ImpactFeedbackManagerComponent)
FBZZ_SCRIPT_ENTRY(sandbox, ResultPresenterComponent)
FBZZ_SCRIPT_ENTRY(sandbox, RumbleManagerComponent)
FBZZ_SCRIPT_ENTRY(sandbox, ScreenEffectManagerComponent)
FBZZ_SCRIPT_ENTRY(sandbox, TimeManagerComponent)
FBZZ_SCRIPT_ENTRY(sandbox, VfxManagerComponent)
FBZZ_SCRIPT_ENTRY(sandbox, AimMarkerComponent)
FBZZ_SCRIPT_ENTRY(sandbox, BeamScorchComponent)
FBZZ_SCRIPT_ENTRY(sandbox, CrosshairComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PlayerAimComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PlayerComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PlayerControllerComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PlayerHeadLookComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PlayerHealthBarComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PlayerHealthComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PolarityGunComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PolarityGunHudComponent)
FBZZ_SCRIPT_ENTRY(sandbox, WeaponAnimatorComponent)
FBZZ_SCRIPT_ENTRY(sandbox, WeaponRigComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PolarityBodyComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PolarityFieldComponent)
FBZZ_SCRIPT_ENTRY(sandbox, PolarityTargetComponent)
FBZZ_SCRIPT_ENTRY(sandbox, SceneManagerScript)
FBZZ_SCRIPT_ENTRY(sandbox, ElectricMinusParticleComponent)
FBZZ_SCRIPT_ENTRY(sandbox, ElectricPlusParticleComponent)
FBZZ_SCRIPT_ENTRY(sandbox, OptionsScreenComponent)
FBZZ_SCRIPT_ENTRY(sandbox, TitleMenuComponent)
FBZZ_SCRIPT_ENTRY(sandbox, GameCursorComponent)
FBZZ_SCRIPT_ENTRY(sandbox, GlowPartComponent)
// @@FBZZ_SCRIPT_ENTRIES_END
