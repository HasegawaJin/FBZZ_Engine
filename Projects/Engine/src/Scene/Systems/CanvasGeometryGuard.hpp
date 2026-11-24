/// @file    CanvasGeometryGuard.hpp
/// @brief   有効な Canvas の部分木を、同じ形状を重ねて調べずに確認する。
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace fbzz::scene::canvas_geometry_detail {

/// @brief 呼び出し単位の作業領域を使い、有効な Canvas の部分木に描画形状があるか調べる。
/// @pre 同期走査の間、シーン階層とコンポーネントが変化しないこと。
/// @note 走査済みで形状のない根だけを記憶し、最初に形状が見つかれば記憶領域を確保せずに返す。
/// @note ビューによる絞り込みより先に UI の計測・配置・ドラッグが姿勢を変更し得る。
/// @note 子はアクティブ状態・有効設定・ビュー条件で絞らず、コンポーネントの有無だけを調べる。
template<typename GeometryProbe>
[[nodiscard]] bool CanShareCanvasFrameGeometry(Scene& scene, GeometryProbe&& hasGeometry)
{
    const auto canvasEntities = scene.GetEntities<UICanvas>();
    const bool deduplicate = canvasEntities.size() > 1;
    std::unordered_set<uint64_t> provenRoots;
    std::vector<GameObject*> pending;
    const auto key = [](const GameObject& object) {
        const EntityID id = object.GetID();
        return (static_cast<uint64_t>(id.generation) << 32) | static_cast<uint64_t>(id.index);
    };
    for (EntityID id : canvasEntities) {
        auto* canvas = scene.GetComponent<UICanvas>(id);
        auto* root = scene.GetGameObject(id);
        if (!canvas || !canvas->enabled || !root || !root->activeInHierarchy())
            continue;
        bool covered = false;
        if (!provenRoots.empty()) {
            for (auto* ancestor = root; ancestor; ancestor = ancestor->GetParent()) {
                if (provenRoots.contains(key(*ancestor))) { covered = true; break; }
            }
        }
        if (covered) continue;
        const bool skipProven = !provenRoots.empty();
        pending.push_back(root);
        while (!pending.empty()) {
            auto* object = pending.back();
            pending.pop_back();
            /// @note 確認済みの子の根は部分木全体を確認済みとして扱えるため、登録順が親からである必要はない。
            if (skipProven && provenRoots.contains(key(*object))) continue;
            if (hasGeometry(*object)) return false;
            for (int childIndex = 0; childIndex < object->GetChildCount(); ++childIndex)
                if (auto* child = object->GetChild(childIndex)) pending.push_back(child);
        }
        if (deduplicate) provenRoots.insert(key(*root));
    }
    return true;
}

/// @brief 無効または空のレンダラーも含め、描画コンポーネントの有無を保守的に調べる。
[[nodiscard]] inline bool CanShareCanvasFrameGeometry(Scene& scene)
{
    return CanShareCanvasFrameGeometry(scene, [](GameObject& object) {
        return object.GetComponent<MeshRenderer>() || object.GetComponent<SkinnedMeshRenderer>();
    });
}

} /// @note namespace fbzz::scene::canvas_geometry_detail
