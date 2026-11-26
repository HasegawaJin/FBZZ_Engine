/// @file    AnimationTrackTable.hpp
/// @brief   プレビューのスケルトンノードとクリップトラックの対応表。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once

#include <cstddef>
#include <vector>

namespace fbzz::asset {
struct AnimationClip;
struct NodeAnimationTrack;
struct Skeleton;
struct SkeletonNode;
}

namespace fbzz::editor {

/// @note トラックは非所有参照。再読み込み後は参照する前に Update を呼ぶ。
class AnimationTrackTable {
public:
    /// @return 対応表を作り直した場合は true。
    /// @note 名前の変更を含む同一アセット内の編集は assetGeneration を進めること。
    bool Update(const asset::Skeleton& skeleton, const asset::AnimationClip* clip,
                int assetGeneration);

    /// @pre nodeIndex は直前の Update に渡した skeleton.nodes の有効な添字。
    [[nodiscard]] const asset::NodeAnimationTrack* operator[](std::size_t nodeIndex) const;

private:
    const asset::Skeleton* m_skeleton = nullptr; ///< 追跡表を作成した Skeleton。アセットが実体を所有する。
    const asset::SkeletonNode* m_nodeData = nullptr; ///< トラックの割り当てを計算した Skeleton のノード配列の先頭。
    const asset::AnimationClip* m_clip = nullptr; ///< 現在索引付けしている AnimationClip。アセットが実体を所有する。
    const asset::NodeAnimationTrack* m_trackData = nullptr; ///< 索引が借用する AnimationClip の元トラック配列の先頭。
    std::size_t m_trackCount = 0; ///< m_trackData のトラック数。索引を作り直すかの判定に使う。
    int m_assetGeneration = -1; ///< 索引の作成時に控えたアセットの世代番号。未作成なら -1。
    std::vector<const asset::NodeAnimationTrack*> m_tracks; ///< スケルトンのノード添字から対応するトラックを引く非所有の表。
};

}
