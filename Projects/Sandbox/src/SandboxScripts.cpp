// FBZZ Engine
// SandboxScripts.cpp | fbzz::sandbox
// Sandbox 固有 Script の ScriptFactory 登録 (EXE 側静的フォールバック)
//
// WHY: #include "Scripts/Foo.hpp" は CMakeLists の include_directories(Assets/) により
//      Assets/Scripts/Foo.hpp に解決される。
//      DLL ホットリロードが有効な場合、これらの登録は ScriptDllLoader が上書きする。
//      DLL 未ビルド時の起動フォールバックとして保持する。
#include "Scripts/PlayerControllerComponent.hpp"
#include "Scripts/PlayerWorldSpaceUIComponent.hpp"
#include "Scripts/TpsCameraComponent.hpp"

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptFactory.hpp>

// WHY: スクリプト追加時に main.cpp を編集しないため、ゲーム側 Translation Unit で自己登録する。
//      Sandbox のスクリプトはヘッダ実装なので、登録専用 .cpp を用意して CMake の GLOB_RECURSE に拾わせる。
FBZZ_REGISTER_SCRIPT(::sandbox::PlayerControllerComponent)
FBZZ_REGISTER_SCRIPT(::sandbox::PlayerWorldSpaceUIComponent)
FBZZ_REGISTER_SCRIPT(::sandbox::TpsCameraComponent)
