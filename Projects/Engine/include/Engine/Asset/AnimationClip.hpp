// FBZZ Engine
// AnimationClip.hpp | fbzz::asset
// Skeletal animation keyframes imported from model files
#pragma once
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <string>
#include <vector>

namespace fbzz::asset {

struct VectorKey {
    double time = 0.0;
    math::Vector3 value = math::Vector3::ZERO;
};

struct QuaternionKey {
    double time = 0.0;
    math::Quaternion value = math::Quaternion::Identity();
};

struct NodeAnimationTrack {
    std::string nodeName;
    std::vector<VectorKey> positions;
    std::vector<QuaternionKey> rotations;
    std::vector<VectorKey> scales;
};

struct AnimationClip {
    std::string name;
    double durationTicks = 0.0;
    double ticksPerSecond = 30.0;
    std::vector<NodeAnimationTrack> tracks;
};

} // namespace fbzz::asset
