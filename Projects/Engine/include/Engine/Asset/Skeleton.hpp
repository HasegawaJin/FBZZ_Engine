// FBZZ Engine
// Skeleton.hpp | fbzz::asset
// スキンメッシュ用の階層ノードとボーン情報
// AnimationClip の nodeName と対応付け、AnimatorSystem が行列パレットを作る。
// インデックス参照を使い、ランタイム中の所有関係は Model 側に集約する。
#pragma once
#include <Math/Matrix4.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::asset {

constexpr int MAX_SKINNING_BONES = 128;

struct SkeletonNode {
    std::string name;
    int parentIndex = -1;
    int boneIndex   = -1; // -1 = 変形に使わない補助ノード (ルート・グループ等)
    math::Matrix4 localBindTransform = math::Matrix4::Identity(); // バインドポーズのローカル TRS
    std::vector<int> children;
};

struct Bone {
    std::string name;
    int nodeIndex = -1;
    // 逆バインドポーズ行列 (inverse bind matrix)。
    // boneMatrix = rootInverse * nodeGlobal * offsetMatrix でスキニング行列を得る。
    math::Matrix4 offsetMatrix = math::Matrix4::Identity();
};

struct Skeleton {
    int rootNodeIndex = -1;
    // メッシュのグローバル変換の逆行列。FBX エクスポート時の座標系を吸収する。
    math::Matrix4 rootInverseTransform = math::Matrix4::Identity();
    std::vector<SkeletonNode> nodes; // 全ノード (ボーン + 補助)
    std::vector<Bone> bones;         // 変形ボーンのみ。GPU パレットのインデックスと対応
    std::unordered_map<std::string, int> nodeMap; // 名前 → nodes インデックス
    std::unordered_map<std::string, int> boneMap; // 名前 → bones インデックス
};

} // namespace fbzz::asset
