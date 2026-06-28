// DemoGame
// EnemyControllerComponent.cpp — ダミー TU (実装は DemoGameScriptsDll.cpp / GameMain.cpp が提供)
//
// WHY: Editor の初回 HotReload では CMake Configure 前の VS プロジェクトが使われ、
//      新規追加した .cpp が Scripts ターゲットにまだ含まれない場合がある。
//      EnemyControllerComponent は DemoGameScriptsDll.cpp で IMPL ブロックを展開し、
//      このファイルは後で GLOB_RECURSE に拾われても多重定義にならないよう実装を封じる。
#define EnemyControllerComponent_IMPL
#include <Engine/Scene/Scene.hpp>
#include "Scripts/EnemyControllerComponent.hpp"
