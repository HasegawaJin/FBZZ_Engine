// FBZZ Engine
// MeshRenderer.hpp | fbzz::scene
// メッシュ参照と表示フラグを持つコンポーネント
// RenderSystem が Transform と組み合わせて DrawCall を発行する。
// GPU リソースは ResourceHandle / shared asset 側に所有させる。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Matrix4.hpp>
#include <cstdint>
#include <limits>
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
    // LOD クロスフェード中のディザしきい値。LODSystem が毎フレーム書き、
    // 描画パスが PerObjectCB へ転送する。0 = 遷移していない。
    float lodDither = 0.0f;
    // シャドウマップへ影を落とすか。
    // WHY: ShadowPass はシーンのジオメトリを光源視点でもう一度描く。地面の装飾・内装の小物・
    //      天井裏のように影が絵に寄与しないオブジェクトを外せるだけで、影の描画量は素直に減る。
    //      既定 true なので、明示的に切らない限り従来と同じ見た目になる。
    bool castShadows = true;

    // "primitive:cube" / "primitive:sphere" / "models/foo.fbx:0"
    std::string meshPath;

    // VelocityPass だけが読み書きする。シーンへは保存しない。
    //
    // WHY 2 枚要るか: Scene View と Game View は同じエンジンフレーム内で VelocityPass を
    //     2 回実行する。1 枚だと 1 回目の更新で「前フレーム」が今フレームに上書きされ、
    //     2 回目のビューの速度が必ず 0 になる。エンジンフレームが変わったときだけ
    //     snapshot を prev へ送り、同フレーム内の 2 回目以降は同じ prev を読む。
    math::Matrix4 prevWorldMatrix     = math::Matrix4::Identity(); // 今フレームが「前」として使う値
    math::Matrix4 worldMatrixSnapshot = math::Matrix4::Identity(); // 次フレームの prevWorldMatrix
    std::uint64_t prevWorldFrame      = (std::numeric_limits<std::uint64_t>::max)();

    const char* GetTypeName() const { return "Mesh Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",     enabled);
        r.Field("castShadows", castShadows);
        r.Field("meshPath",    meshPath);
    }
};

} // namespace fbzz::scene
