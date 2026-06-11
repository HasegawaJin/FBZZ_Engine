// FBZZ Engine
// PrefabRef.hpp | fbzz::scene
// Prefab アセット参照型。IReflector::Field(PrefabRef&) でシリアライズ・Inspector 表示される。
// ScriptSceneProxy が Instantiate を宣言するために ScriptProxy より早く定義が必要なため独立ヘッダとする。
#pragma once
#include <string>

namespace fbzz::scene {

struct PrefabRef {
    std::string path;
    bool operator==(const PrefabRef&) const = default;
};

} // namespace fbzz::scene
