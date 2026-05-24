// FBZZ Engine
// MeshRenderer.hpp | fbzz::scene
// メッシュ参照と表示フラグを持つコンポーネント
// RenderSystem が Transform と組み合わせて DrawCall を発行する。
// GPU リソースは ResourceHandle / shared asset 側に所有させる。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <memory>
#include <string>

namespace fbzz::renderer {
struct Mesh;
} // namespace fbzz::renderer

namespace fbzz::scene {

struct MeshRenderer {
    std::shared_ptr<renderer::Mesh> mesh;
    bool enabled = true;

    // "primitive:cube" / "primitive:sphere" / "models/foo.fbx:0"
    std::string meshPath;

    const char* GetTypeName() const { return "Mesh Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",  enabled);
        r.Field("meshPath", meshPath);
    }
};

} // namespace fbzz::scene
