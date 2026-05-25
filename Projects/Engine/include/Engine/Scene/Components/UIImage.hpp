// FBZZ Engine
// UIImage.hpp | fbzz::scene
// UI スプライト描画コンポーネント
// 位置とサイズは GameObject::transform の local 値から読む。
// Texture は ResourceHandle で参照し、AssetManager / ResourceManager が所有する。
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Math/Vector4.hpp>
#include <Math/Vector2.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>

namespace fbzz::scene {

struct UIImage {
    std::string   texturePath = "";
    renderer::ResourceHandle<renderer::TextureTag> texture = {};
    math::Vector4 color  = { 1.0f, 1.0f, 1.0f, 1.0f };
    math::Vector2 uvMin  = { 0.0f, 0.0f };
    math::Vector2 uvMax  = { 1.0f, 1.0f };
    bool          enabled = true;

    const char* GetTypeName() const { return "UIImage"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",     enabled);
        r.Field("texturePath", texturePath);
        r.Field("color",       color);
        r.Field("uvMin",       uvMin);
        r.Field("uvMax",       uvMax);
    }
};

} // namespace fbzz::scene
