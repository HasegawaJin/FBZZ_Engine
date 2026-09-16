/// @file    LODGroupComponent.hpp
/// @brief   カメラ上の相対表示サイズに応じて Renderer 群を切り替える LOD 定義。
/// @author  Hasegawa Jin
/// @date    2026-07-15
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

    // レベルを入れ替えるときのディザクロスフェード秒数。0 で従来どおり即差し替え。
    // WHY 既定を短くするか: 長くすると、まだら模様が TAA で溶けきる前に視点が動いて
    //     「メッシュが 2 つ重なっている」状態が見えてしまう。
    float fadeDuration = 0.25f;

    std::vector<LODLevel> levels;

    // ランタイム状態。Scene には保存しない (Play/Stop で遷移が復元される必要はない)。
    int   activeLevel = -1;   // 今フレーム表示すべきレベル。-1 は未決定
    int   fadingLevel = -1;   // 退場中のレベル。-1 は遷移していない
    float fadeElapsed = 0.0f;

    const char* GetTypeName() const { return "LOD Group"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("size", size);
        r.Field("cullBelowLastLevel", cullBelowLastLevel);
        r.Field("fadeDuration", fadeDuration);
        // levels は入れ子 vector のため SceneSerializer が専用コードで保存する。
    }
};

} // namespace fbzz::scene
