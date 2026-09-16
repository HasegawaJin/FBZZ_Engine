/// @file    Skeleton.hpp
/// @brief   スキンメッシュ用の階層ノードとボーン情報。
/// @author  Hasegawa Jin
/// @date    2026-05-24
///
/// AnimationClip の nodeName と対応付け、AnimatorSystem が行列パレットを構築する。
/// インデックス配列を使い、ランタイム中の動的確保を減らす。Model 側に保持される。
#pragma once
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::asset {

constexpr int MAX_SKINNING_BONES = 128;

struct SkeletonNode {
    std::string name;
    int parentIndex = -1;
    int boneIndex   = -1; // -1 = 変形に使わない補助ノード (ルート・グループ等)
    math::Vector3 bindTranslation = math::Vector3::ZERO;
    math::Quaternion bindRotation = math::Quaternion::Identity();
    math::Vector3 bindScale = math::Vector3::ONE;
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
    // メッシュのグローバル変換の逆行列。FBX エクスポート時の座標補正を打ち消す。
    math::Matrix4 rootInverseTransform = math::Matrix4::Identity();
    std::vector<SkeletonNode> nodes; // 全ノード (ボーン + 補助)
    std::vector<Bone> bones;         // 変形ボーンのみ。GPU パレットのインデックスと対応
    std::unordered_map<std::string, int> nodeMap; // 名前 -> nodes インデックス
    std::unordered_map<std::string, int> boneMap; // 名前 -> bones インデックス

    // リファレンスポーズ (= バインドポーズ) のスキニング行列パレット。
    // bones と同じインデックス。BuildReferencePose() がロード時に一度だけ構築する。
    //
    // WHY: 「アニメーションが無いときのスキニング行列」は単位行列ではない。
    //   単位行列が正しくなるのは、頂点がモデル空間そのままで格納されている
    //   アセットに限られる (Mixamo など)。ノード階層にバインド変換を持つアセットでは
    //   モデルが倒れる・原点へ潰れるといった破綻が起きる。
    //   Unity / Unreal と同じく、無アニメ時はこのリファレンスポーズを既定とする。
    std::vector<math::Matrix4> referencePose;
};

// nodes のバインド TRS を辿って referencePose を構築する。
// palette[boneIndex] = rootInverseTransform · globalBind · offsetMatrix
void BuildReferencePose(Skeleton& skeleton);

} // namespace fbzz::asset
