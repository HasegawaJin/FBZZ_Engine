// DemoGame
// PlayerIKComponent.cpp — ダミー TU (実装は DemoGameScriptsDll.cpp が提供)
//
// WHY: PlayerIKComponent の IMPL ブロックは scene.GetScript<PlayerControllerComponent>() を
//      使うため PlayerControllerComponent の完全型が必要。
//      DemoGameScriptsDll.cpp では PlayerControllerComponent.hpp (_IMPL 定義済み) を先に
//      インクルードしてから PlayerIKComponent.hpp を続けることで完全型を確保し、
//      そこで IMPL ブロックを走らせている。
//      このファイルは CMake の GLOB_RECURSE が将来拾った際に多重定義 (ODR 違反) に
//      ならないよう _IMPL を先に定義して実装ブロックを封じるダミーとして置いている。
#define PlayerIKComponent_IMPL
#include <Engine/Scene/Scene.hpp>
#define PlayerControllerComponent_IMPL
#include "Scripts/PlayerControllerComponent.hpp"
#include "Scripts/PlayerIKComponent.hpp"
