/// @file    AssimpNodeExport.hpp
/// @brief   モデル・スケルトン書き出しの行列変換とノード順序を共用する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <assimp/scene.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::editor::fbxexport {

/// @note 平行移動だけをunitScale倍し、FZND/FZSKが共有する行順の16要素へ保存する。
inline void CopyMatrix(const aiMatrix4x4& source, float destination[16], float unitScale = 1.0f)
{
    destination[0] = source.a1; destination[1] = source.a2; destination[2] = source.a3; destination[3] = source.a4 * unitScale;
    destination[4] = source.b1; destination[5] = source.b2; destination[6] = source.b3; destination[7] = source.b4 * unitScale;
    destination[8] = source.c1; destination[9] = source.c2; destination[10] = source.c3; destination[11] = source.c4 * unitScale;
    destination[12] = source.d1; destination[13] = source.d2; destination[14] = source.d3; destination[15] = source.d4;
}

/// @note 出力型の既定値を保ち、先行順の添字と同名ノードの最終登録を両形式で一致させる。
template<typename NodeEntry>
void TraverseNodes(const aiNode* node, int parentIndex, float unitScale,
                   std::unordered_map<std::string, int>& indexMap,
                   std::vector<NodeEntry>& nodes)
{
    const int currentIndex = static_cast<int>(nodes.size());
    nodes.emplace_back();
    auto& entry = nodes.back();
    entry.name = node->mName.C_Str();
    entry.parentIndex = parentIndex;
    aiVector3D position, scale;
    aiQuaternion rotation;
    node->mTransformation.Decompose(scale, rotation, position);
    entry.bindT[0] = position.x * unitScale; entry.bindT[1] = position.y * unitScale; entry.bindT[2] = position.z * unitScale;
    entry.bindR[0] = rotation.x; entry.bindR[1] = rotation.y; entry.bindR[2] = rotation.z; entry.bindR[3] = rotation.w;
    entry.bindS[0] = scale.x; entry.bindS[1] = scale.y; entry.bindS[2] = scale.z;
    CopyMatrix(node->mTransformation, entry.localTransform, unitScale);
    indexMap[entry.name] = currentIndex;
    if (parentIndex >= 0) nodes[static_cast<size_t>(parentIndex)].children.push_back(currentIndex);
    for (uint32_t index = 0; index < node->mNumChildren; ++index)
        TraverseNodes(node->mChildren[index], currentIndex, unitScale, indexMap, nodes);
}

} /// @note namespace fbzz::editor::fbxexport
