/// @file    Skeleton.hpp
/// @brief   スキンメッシュ用の階層ノードとボーン情報。
/// @author  Hasegawa Jin
/// @date    2026-05-24

/// @note AnimationClip の nodeName と対応付け、AnimatorSystem が行列パレットを構築する。
/// @note インデックス配列を使い、ランタイム中の動的確保を減らす。Model 側に保持される。
#pragma once
#include <Engine/Api.hpp>
#include <Math/LinearAlgebra/Matrix4.hpp>
#include <Math/LinearAlgebra/Quaternion.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::asset {

constexpr int MAX_SKINNING_BONES = 128;

struct SkeletonNode {
    std::string name; ///< アニメーションのトラックから照合する骨格ノードの名前。
    int parentIndex = -1; ///< 骨格ノード配列の親の添字。-1 は親のないルート。
    int boneIndex   = -1; ///< -1 = 変形に使わない補助ノード (ルート・グループ等)
    math::Vector3 bindTranslation = math::Vector3::ZERO; ///< バインド姿勢における親ノード基準の位置。
    math::Quaternion bindRotation = math::Quaternion::Identity(); ///< バインド姿勢における親ノード基準の回転。
    math::Vector3 bindScale = math::Vector3::ONE; ///< バインド姿勢における親ノード基準の軸別倍率。
    math::Matrix4 localBindTransform = math::Matrix4::Identity(); ///< バインドポーズのローカル TRS
    std::vector<int> children; ///< 直接の子に当たる骨格ノード配列の添字列。
};

struct Bone {
    std::string name; ///< スキニングで使用する骨の名前。
    int nodeIndex = -1; ///< 出力骨に対応する骨格ノード配列の添字。-1 は未設定。
    /// @note 逆バインドポーズ行列 (inverse bind matrix)。
    /// @note boneMatrix = rootInverse * nodeGlobal * offsetMatrix でスキニング行列を得る。
    math::Matrix4 offsetMatrix = math::Matrix4::Identity(); ///< 頂点をバインド姿勢の骨空間へ移す逆バインド行列。
};

struct Skeleton {
    int rootNodeIndex = -1; ///< 骨格ノード配列のルートの添字。-1 は未設定。
    math::Matrix4 rootInverseTransform = math::Matrix4::Identity(); ///< メッシュのグローバル変換の逆行列。FBX エクスポート時の座標補正を打ち消す。
    std::vector<SkeletonNode> nodes; ///< 全ノード (ボーン + 補助)
    std::vector<Bone> bones; ///< 変形ボーンのみ。GPU パレットのインデックスと対応
    std::unordered_map<std::string, int> nodeMap; ///< 名前 -> nodes インデックス
    std::unordered_map<std::string, int> boneMap; ///< 名前 -> bones インデックス

    /// @note リファレンスポーズ (= バインドポーズ) のスキニング行列パレット。
    /// @note bones と同じインデックス。BuildReferencePose() がロード時に一度だけ構築する。

    /// @note 単位行列は正しくない: 頂点がモデル空間そのまま格納されるアセット (Mixamo 等) 以外は、
    /// @note モデルが倒れる・原点へ潰れるといった破綻が起きる。Unity / Unreal と同じく無アニメ時は
    /// @note このリファレンスポーズを既定とする。
    std::vector<math::Matrix4> referencePose; ///< 未再生時に使用する基準姿勢のスキニング行列列。
};

/// @note nodes のバインド TRS を辿って referencePose を構築する。
/// @note palette[boneIndex] = rootInverseTransform · globalBind · offsetMatrix
FBZZ_ENGINE_API void BuildReferencePose(Skeleton& skeleton);

} // namespace fbzz::asset
