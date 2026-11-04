/// @file    Skeleton.hpp
/// @brief   スキンメッシュ用の階層ノードとボーン情報。
/// @author  Hasegawa Jin
/// @date    2026-05-24

/// @note AnimationClip の nodeName と対応付け、AnimatorSystem が行列パレットを構築する。
/// @note インデックス配列を使い、ランタイム中の動的確保を減らす。Model 側に保持される。
#pragma once
#include <Engine/Api.hpp>
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
    int boneIndex   = -1; ///< @note -1 = 変形に使わない補助ノード (ルート・グループ等)
    math::Vector3 bindTranslation = math::Vector3::ZERO;
    math::Quaternion bindRotation = math::Quaternion::Identity();
    math::Vector3 bindScale = math::Vector3::ONE;
    math::Matrix4 localBindTransform = math::Matrix4::Identity(); ///< @note バインドポーズのローカル TRS
    std::vector<int> children;
};

struct Bone {
    std::string name;
    int nodeIndex = -1;
    /// @note 逆バインドポーズ行列 (inverse bind matrix)。
    /// @note boneMatrix = rootInverse * nodeGlobal * offsetMatrix でスキニング行列を得る。
    math::Matrix4 offsetMatrix = math::Matrix4::Identity();
};

struct Skeleton {
    int rootNodeIndex = -1;
    /// @note メッシュのグローバル変換の逆行列。FBX エクスポート時の座標補正を打ち消す。
    math::Matrix4 rootInverseTransform = math::Matrix4::Identity();
    std::vector<SkeletonNode> nodes; ///< @note 全ノード (ボーン + 補助)
    std::vector<Bone> bones;         ///< @note 変形ボーンのみ。GPU パレットのインデックスと対応
    std::unordered_map<std::string, int> nodeMap; ///< @note 名前 -> nodes インデックス
    std::unordered_map<std::string, int> boneMap; ///< @note 名前 -> bones インデックス

    /// @note リファレンスポーズ (= バインドポーズ) のスキニング行列パレット。
    /// @note bones と同じインデックス。BuildReferencePose() がロード時に一度だけ構築する。

    /// @note 単位行列は正しくない: 頂点がモデル空間そのまま格納されるアセット (Mixamo 等) 以外は、
    /// @note モデルが倒れる・原点へ潰れるといった破綻が起きる。Unity / Unreal と同じく無アニメ時は
    /// @note このリファレンスポーズを既定とする。
    std::vector<math::Matrix4> referencePose;
};

/// @note nodes のバインド TRS を辿って referencePose を構築する。
/// @note palette[boneIndex] = rootInverseTransform · globalBind · offsetMatrix
FBZZ_ENGINE_API void BuildReferencePose(Skeleton& skeleton);

} /// @note namespace fbzz::asset
