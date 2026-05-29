// FBZZ Engine
// BoneComponent.hpp | fbzz::scene
// シーンの GameObject とインポート済み SkeletonNode を 1 対 1 で対応付けるコンポーネント。
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>

namespace fbzz::scene {

struct BoneComponent {
    std::string boneName;
    int nodeIndex = -1;
    int boneIndex = -1;
    EntityID skinnedMeshEntity = EntityID::INVALID;
    bool generated = true;

    const char* GetTypeName() const { return "Bone"; }
    void Reflect(IReflector& r)
    {
        r.Field("boneName", boneName);
        r.Field("nodeIndex", nodeIndex);
        r.Field("boneIndex", boneIndex);
        r.Field("generated", generated);
    }
};

} // namespace fbzz::scene
