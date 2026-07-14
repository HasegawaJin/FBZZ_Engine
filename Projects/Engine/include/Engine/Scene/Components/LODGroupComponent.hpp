// FBZZ Engine
// LODGroupComponent.hpp | fbzz::scene
// カメラ上の相対表示サイズに応じて Renderer 群を切り替える LOD 定義
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>
#include <vector>

namespace fbzz::scene {

// LODRendererReference — Scene 保存に安定な instanceId とランタイム EntityID を併記する。
// WHY: EntityID は Scene ロードごとに変化するため、永続化には GameObject::instanceId を使う。
struct LODRendererReference {
    std::string instanceId;
    EntityID entity = EntityID::INVALID;
};

// LODLevel — screenRelativeHeight 以上のとき、このレベルの Renderer を表示する。
// レベルは高品質から低品質へ閾値の降順で並べる。
struct LODLevel {
    float screenRelativeHeight = 0.5f;
    std::vector<LODRendererReference> renderers;
};

// LODGroupComponent — Unity の LODGroup に相当する切り替え単位。
struct LODGroupComponent {
    bool enabled = true;
    float size = 1.0f;
    bool cullBelowLastLevel = false;
    std::vector<LODLevel> levels;

    const char* GetTypeName() const { return "LOD Group"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("size", size);
        r.Field("cullBelowLastLevel", cullBelowLastLevel);
        // levels は入れ子 vector のため SceneSerializer が専用コードで保存する。
    }
};

} // namespace fbzz::scene
