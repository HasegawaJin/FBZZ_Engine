/// @file    MeshRenderer.hpp
/// @brief   メッシュ参照と表示フラグを持つコンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// @note RenderSystem が Transform と組み合わせて DrawCall を発行する。
/// @note GPU リソースは ResourceHandle / shared asset 側に所有させる。
#pragma once
#include <Engine/Scene/Reflection.hpp>
#include <Math/Matrix4.hpp>
#include <cstdint>
#include <limits>
#include <string>

namespace fbzz::renderer {
struct Mesh;
} /// @note namespace fbzz::renderer

namespace fbzz::scene {

struct MeshRenderer {
    renderer::Mesh* mesh = nullptr;
    bool enabled = true;
    /// @note LODSystem 専用のランタイム可視性。enabled と分離してユーザー設定を保持する。
    bool lodVisible = true;
    /// @note LOD クロスフェード中のディザしきい値。LODSystem が毎フレーム書き、
    /// @note 描画パスが PerObjectCB へ転送する。0 = 遷移していない。
    float lodDither = 0.0f;
    /// @note シャドウマップへ影を落とすか。
    /// @note ShadowPass はシーンを光源視点でもう一度描くため、影に寄与しないオブジェクトを
    /// @note 外すだけで描画量が減る。既定 true で従来と同じ見た目を保つ。
    bool castShadows = true;

    /// @note "primitive:cube" / "primitive:sphere" / "models/foo.fbx:0"
    std::string meshPath;

    /// @note meshPath を書き換えたので mesh を引き直してほしい、という要求。
    /// @note RuntimeMeshSystem が Phase::LateUpdate で処理して false へ戻す。シーンへは保存しない。
    /// @note mesh の解決には ResourceManager が要るが、SystemContext がそれを持つのは LateUpdate だけ。
    /// @note フラグが無いと文字列だけ変わって形が変わらず、実行中の差し替えが黙って効かない。
    bool meshPathDirty = false;

    /// @note VelocityPass だけが読み書きする。シーンへは保存しない。
    ///
    /// @note Scene View / Game View は同じエンジンフレーム内で VelocityPass を 2 回実行する。
    /// @note 1 枚だと 1 回目の更新で「前フレーム」が上書きされ 2 回目の速度が必ず 0 になるため、
    /// @note エンジンフレームが変わったときだけ snapshot を prev へ送る。
    math::Matrix4 prevWorldMatrix     = math::Matrix4::Identity(); ///< @note 今フレームが「前」として使う値
    math::Matrix4 worldMatrixSnapshot = math::Matrix4::Identity(); ///< @note 次フレームの prevWorldMatrix
    std::uint64_t prevWorldFrame      = (std::numeric_limits<std::uint64_t>::max)();

    const char* GetTypeName() const { return "Mesh Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",     enabled);
        r.Field("castShadows", castShadows);
        r.Field("meshPath",    meshPath);
    }
};

} /// @note namespace fbzz::scene
