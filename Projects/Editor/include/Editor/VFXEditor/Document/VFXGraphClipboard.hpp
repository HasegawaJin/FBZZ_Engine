// FBZZ Engine
// VFXGraphClipboard.hpp | fbzz::editor
// ノード集合のアプリ内クリップボード
#pragma once

#include <Engine/Asset/VFXGraphAsset.hpp>
#include <vector>

namespace fbzz::editor {

// コピーしたノード群と、その集合の内部だけで閉じたリンクを保持する。
// WHY: 集合の外へ出ていくリンクまで持つと、貼り付け先に相手ノードが存在せず
//      「切れたワイヤーだけが増える」結果になる。内部リンクに限定して整合を保つ。
// NOTE: 座標は貼り付け時に基準点からの相対で置き直すため、ここでは元の
//       Grid 座標のまま保持する (相対化は貼り付け側の責務)。
struct VFXGraphClipboard {
    std::vector<asset::VFXGraphNode> nodes;
    std::vector<asset::VFXGraphLink> links;

    [[nodiscard]] bool HasContent() const { return !nodes.empty(); }

    void Clear()
    {
        nodes.clear();
        links.clear();
    }
};

} // namespace fbzz::editor
