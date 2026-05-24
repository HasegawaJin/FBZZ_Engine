// FBZZ Engine
// Skeleton.hpp | fbzz::asset
// Skeletal hierarchy and bind-pose data for skinned models
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
    int boneIndex = -1;
    math::Matrix4 localBindTransform = math::Matrix4::Identity();
    std::vector<int> children;
};

struct Bone {
    std::string name;
    int nodeIndex = -1;
    math::Matrix4 offsetMatrix = math::Matrix4::Identity();
};

struct Skeleton {
    int rootNodeIndex = -1;
    math::Matrix4 rootInverseTransform = math::Matrix4::Identity();
    std::vector<SkeletonNode> nodes;
    std::vector<Bone> bones;
    std::unordered_map<std::string, int> nodeMap;
    std::unordered_map<std::string, int> boneMap;
};

} // namespace fbzz::asset
