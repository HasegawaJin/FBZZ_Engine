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
    const asset::Skeleton* m_skeleton = nullptr;
    const asset::SkeletonNode* m_nodeData = nullptr;
    const asset::AnimationClip* m_clip = nullptr;
    const asset::NodeAnimationTrack* m_trackData = nullptr;
    std::size_t m_trackCount = 0;
    int m_assetGeneration = -1;
    std::vector<const asset::NodeAnimationTrack*> m_tracks;
};

}
