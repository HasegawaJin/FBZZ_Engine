// FBZZ Engine
// SkinnedMeshRenderer.hpp | fbzz::scene
// スキンメッシュ描画用コンポーネント
// Model の mesh / skeleton と Animator の骨行列を使って描画する。
// 静的 MeshRenderer と分け、スキニング固有の状態をここに集める。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Engine/Asset/Model.hpp>
#include <memory>
#include <string>

namespace fbzz::scene {

struct SkinnedMeshRenderer {
    bool enabled = true;
    std::string modelPath;
    int meshIndex = 0;

    std::shared_ptr<asset::Model> model; // ランタイム専用。シリアライズしない

    const char* GetTypeName() const { return "Skinned Mesh Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",   enabled);
        r.Field("modelPath", modelPath);
        r.Field("meshIndex", meshIndex);
    }
};

} // namespace fbzz::scene
