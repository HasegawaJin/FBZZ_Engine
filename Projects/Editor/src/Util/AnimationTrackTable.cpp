/// @file    AnimationTrackTable.cpp
/// @brief   FBX の名前規約を保ってプレビュー用トラック対応表を構築する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Editor/Util/AnimationTrackTable.hpp>

#include <Engine/Asset/AnimationClip.hpp>
#include <Engine/Asset/Skeleton.hpp>

#include <algorithm>
#include <cassert>
#include <string>
#include <string_view>
#include <unordered_map>

namespace fbzz::editor {
namespace {

/// @note AnimatorSystem の照合と同じく FBX 補助ノード suffix、階層パス、namespace を取り除く。
std::string CanonicalNodeName(std::string name)
{
    std::replace(name.begin(), name.end(), '\\', '/');
    if (const std::size_t helper = name.find("_$AssimpFbx$_"); helper != std::string::npos)
        name.resize(helper);
    if (const std::size_t path = name.find_last_of("/|"); path != std::string::npos)
        name.erase(0, path + 1);
    if (const std::size_t nameSpace = name.find_last_of(':'); nameSpace != std::string::npos)
        name.erase(0, nameSpace + 1);
    return name;
}

}

bool AnimationTrackTable::Update(const asset::Skeleton& skeleton,
                                 const asset::AnimationClip* clip,
                                 int assetGeneration)
{
    const auto* trackData = clip ? clip->tracks.data() : nullptr;
    const std::size_t trackCount = clip ? clip->tracks.size() : 0;
    if (m_skeleton == &skeleton && m_nodeData == skeleton.nodes.data()
        && m_tracks.size() == skeleton.nodes.size() && m_clip == clip
        && m_trackData == trackData && m_trackCount == trackCount
        && m_assetGeneration == assetGeneration)
        return false;

    m_skeleton = &skeleton;
    m_nodeData = skeleton.nodes.data();
    m_clip = clip;
    m_trackData = trackData;
    m_trackCount = trackCount;
    m_assetGeneration = assetGeneration;
    m_tracks.assign(skeleton.nodes.size(), nullptr);
    if (!clip) return true;

    std::unordered_map<std::string_view, const asset::NodeAnimationTrack*> exactTracks;
    std::unordered_map<std::string, const asset::NodeAnimationTrack*> canonicalTracks;
    exactTracks.reserve(trackCount);
    canonicalTracks.reserve(trackCount);
    /// @note 完全一致が正規化一致より優先。同名トラックが複数ある場合は元の配列の先頭を使う。
    for (const auto& track : clip->tracks) {
        exactTracks.try_emplace(track.nodeName, &track);
        canonicalTracks.try_emplace(CanonicalNodeName(track.nodeName), &track);
    }
    for (std::size_t index = 0; index < skeleton.nodes.size(); ++index) {
        const auto& name = skeleton.nodes[index].name;
        if (const auto found = exactTracks.find(name); found != exactTracks.end()) {
            m_tracks[index] = found->second;
        } else if (const auto canonicalFound = canonicalTracks.find(CanonicalNodeName(name));
                   canonicalFound != canonicalTracks.end()) {
            m_tracks[index] = canonicalFound->second;
        }
    }
    return true;
}

const asset::NodeAnimationTrack* AnimationTrackTable::operator[](std::size_t nodeIndex) const
{
    assert(nodeIndex < m_tracks.size());
    return m_tracks[nodeIndex];
}

}
