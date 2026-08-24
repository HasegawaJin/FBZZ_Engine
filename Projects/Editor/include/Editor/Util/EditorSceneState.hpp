// FBZZ Engine
// EditorSceneState.hpp | fbzz::editor
// Scene に紐づく Editor 専用メタデータ
#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fbzz::editor {

// ランタイム Scene へ混ぜない、Editor 上の Scene 状態。
// WHY: Inspector の表示順はゲーム実行時のデータではなく、Editor の編集体験だけに属する。
//      この型を Engine から分離することで、Standalone が Editor メタデータを読まずに済む。
struct EditorSceneState {
    using ComponentOrder = std::vector<std::string>;
    using InstanceIds    = std::vector<std::string>;

    void Clear()
    {
        componentOrders.clear();
        hiddenObjects.clear();
        lockedObjects.clear();
    }

    // Hierarchy の非表示 (👁) / ロック (🔒) を付けた GameObject の instanceId。
    // WHY EntityID ではなく instanceId か: EntityID はシーンを読み直すたびに振り直され、
    //     しかも別シーンで同じ値が使われる。番号で覚えると、シーンを開き直した瞬間に
    //     まったく別のオブジェクトがロックされ、「選べないのに理由が分からない」状態になる。
    const InstanceIds& GetHiddenObjects() const { return hiddenObjects; }
    void SetHiddenObjects(InstanceIds ids) { hiddenObjects = std::move(ids); }
    const InstanceIds& GetLockedObjects() const { return lockedObjects; }
    void SetLockedObjects(InstanceIds ids) { lockedObjects = std::move(ids); }

    const ComponentOrder& GetComponentOrder(std::string_view instanceId) const
    {
        const auto it = componentOrders.find(std::string(instanceId));
        if (it != componentOrders.end()) return it->second;
        static const ComponentOrder empty;
        return empty;
    }

    void EnsureComponentOrder(std::string_view instanceId, std::string_view componentKey)
    {
        if (instanceId.empty() || componentKey.empty()) return;
        auto& order = componentOrders[std::string(instanceId)];
        if (std::find(order.begin(), order.end(), componentKey) == order.end())
            order.emplace_back(componentKey);
    }

    // draggedKey を targetKey の直前 / 直後へ差し込む。
    //
    // WHY insertAfter を受けるか (不具合修正): 以前は常に「直前へ挿入」だった。
    //     どのカードへ落としても手前にしか入らないため、末尾のカードより後ろへは
    //     何も動かせず、最後の Component が固定されて並び替え不能になっていた。
    bool MoveComponentOrder(std::string_view instanceId,
                            std::string_view draggedKey,
                            std::string_view targetKey,
                            bool insertAfter)
    {
        if (draggedKey.empty() || targetKey.empty() || draggedKey == targetKey) return false;
        EnsureComponentOrder(instanceId, draggedKey);
        EnsureComponentOrder(instanceId, targetKey);

        auto& order = componentOrders[std::string(instanceId)];
        const auto dragged = std::find(order.begin(), order.end(), draggedKey);
        if (dragged == order.end()) return false;

        const std::string value = *dragged;
        // 先に抜いてから挿入位置を引き直す。抜く前に求めた iterator は erase で無効になる。
        order.erase(dragged);
        auto target = std::find(order.begin(), order.end(), targetKey);
        if (target == order.end()) return false;
        if (insertAfter) ++target;   // end() への ++ は起きない (target は必ず有効要素を指す)
        order.insert(target, value);
        return true;
    }

    // 実際に描画された Component のキーだけを残す。
    //
    // WHY 要るか (不具合修正): EnsureComponentOrder は描画のたびにキーを追記する一方、
    //     取り除く経路がどこにも無かった。Component を削除しても、リネームしても
    //     古いキーが残り続け、そのまま .meta へ書き出されて増え続けていた。
    //     残骸は並び順の途中に「存在しない席」を作るため、見た目の並びと
    //     保存された並びが少しずつ食い違っていく原因にもなる。
    void PruneComponentOrder(std::string_view instanceId,
                             const std::vector<std::string>& presentKeys)
    {
        if (instanceId.empty()) return;
        const auto it = componentOrders.find(std::string(instanceId));
        if (it == componentOrders.end()) return;

        const std::unordered_set<std::string> present(presentKeys.begin(), presentKeys.end());
        ComponentOrder& order = it->second;
        order.erase(std::remove_if(order.begin(), order.end(),
                                   [&present](const std::string& key) {
                                       return !present.contains(key);
                                   }),
                    order.end());
        if (order.empty()) componentOrders.erase(it);
    }

    void SetComponentOrder(std::string_view instanceId, ComponentOrder order)
    {
        if (instanceId.empty()) return;
        ComponentOrder unique;
        unique.reserve(order.size());
        for (std::string& key : order) {
            if (key.empty() || std::find(unique.begin(), unique.end(), key) != unique.end()) continue;
            unique.push_back(std::move(key));
        }
        componentOrders[std::string(instanceId)] = std::move(unique);
    }

    void CopyComponentOrder(std::string_view sourceInstanceId,
                            std::string_view targetInstanceId)
    {
        if (sourceInstanceId.empty() || targetInstanceId.empty()) return;
        const auto it = componentOrders.find(std::string(sourceInstanceId));
        if (it == componentOrders.end()) {
            componentOrders.erase(std::string(targetInstanceId));
            return;
        }
        componentOrders[std::string(targetInstanceId)] = it->second;
    }

    void PruneToInstances(const std::vector<std::string>& instanceIds)
    {
        const std::unordered_set<std::string> valid(instanceIds.begin(), instanceIds.end());
        for (auto it = componentOrders.begin(); it != componentOrders.end();) {
            if (!valid.contains(it->first)) it = componentOrders.erase(it);
            else ++it;
        }
        const auto pruneIds = [&valid](InstanceIds& ids) {
            ids.erase(std::remove_if(ids.begin(), ids.end(),
                                     [&valid](const std::string& id) { return !valid.contains(id); }),
                      ids.end());
        };
        pruneIds(hiddenObjects);
        pruneIds(lockedObjects);
    }

    const std::unordered_map<std::string, ComponentOrder>& GetAllComponentOrders() const
    {
        return componentOrders;
    }

private:
    std::unordered_map<std::string, ComponentOrder> componentOrders;
    InstanceIds hiddenObjects;
    InstanceIds lockedObjects;
};

} // namespace fbzz::editor
