/// @file    BoneComponent.hpp
/// @brief   シーンの GameObject とインポート済み SkeletonNode を 1 対 1 で対応付けるコンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-30
#pragma once

#include <Engine/Scene/References/Entity.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <string>

namespace fbzz::scene {

struct BoneComponent {
    std::string boneName; ///< 所属スケルトン内のボーンを照合する名前。
    int nodeIndex = -1; ///< 所属スケルトンのノード配列の添字。-1 は未解決。
    int boneIndex = -1; ///< スキニングパレット上のボーンの添字。-1 は対応するパレット要素なし。
    EntityID skinnedMeshEntity = EntityID::INVALID; ///< このボーンを利用するスキンドメッシュのエンティティ ID。
    bool generated = true; ///< モデルから自動生成されたボーンのエンティティかどうか。

    const char* GetTypeName() const { return "Bone"; }
    void Reflect(IReflector& r)
    {
        r.Field("boneName", boneName);
        r.Field("nodeIndex", nodeIndex);
        r.Field("boneIndex", boneIndex);
        r.Field("generated", generated);
    }
};

}
