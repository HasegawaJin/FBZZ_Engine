/// @file    PrefabRef.hpp
/// @brief   Prefab アセット参照型。IReflector::Field(PrefabRef&) でシリアライズ・Inspector 表示される。
/// @author  Hasegawa Jin
/// @date    2026-06-10
///
/// ScriptSceneProxy が Instantiate を宣言するために ScriptProxy より早く定義が必要なため独立ヘッダとする。
#pragma once
#include <string>

namespace fbzz::scene {

struct PrefabRef {
    std::string path;
    bool operator==(const PrefabRef&) const = default;
};

} // namespace fbzz::scene
