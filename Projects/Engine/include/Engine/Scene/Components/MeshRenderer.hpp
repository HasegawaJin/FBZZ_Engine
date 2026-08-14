// FBZZ Engine
// MeshRenderer.hpp | fbzz::scene
// メッシュ参照と表示フラグを持つコンポーネント
// RenderSystem が Transform と組み合わせて DrawCall を発行する。
// GPU リソースは ResourceHandle / shared asset 側に所有させる。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <string>

namespace fbzz::renderer {
struct Mesh;
} // namespace fbzz::renderer

namespace fbzz::scene {

struct MeshRenderer {
    renderer::Mesh* mesh = nullptr;
    bool enabled = true;
    // LODSystem 専用のランタイム可視性。enabled と分離してユーザー設定を保持する。
    bool lodVisible = true;
    // シャドウマップへ影を落とすか。
    // WHY: ShadowPass はシーンのジオメトリを光源視点でもう一度描く。地面の装飾・内装の小物・
    //      天井裏のように影が絵に寄与しないオブジェクトを外せるだけで、影の描画量は素直に減る。
    //      既定 true なので、明示的に切らない限り従来と同じ見た目になる。
    bool castShadows = true;

    // "primitive:cube" / "primitive:sphere" / "models/foo.fbx:0"
    std::string meshPath;

    const char* GetTypeName() const { return "Mesh Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",     enabled);
        r.Field("castShadows", castShadows);
        r.Field("meshPath",    meshPath);
    }
};

} // namespace fbzz::scene
