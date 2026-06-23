// FBZZ Engine
// CameraComponent.hpp | fbzz::scene
// カメラパラメータを持つコンポーネント
// 位置と回転は GameObject の Transform を正とし、この型は投影設定を持つ。
// RenderSystem が Camera へ変換して描画に使う。
#pragma once
#include <Physics/Layer.hpp>
#include <Engine/Scene/Script.hpp>

namespace fbzz::scene {

struct CameraComponent {
    float fovY    = 60.0f;
    // WHY: RenderSystem 呼び出し元の viewport aspect と CameraComponent の投影設定を同期し、
    //      Script の座標変換 API でも同じ投影行列を再現できるように Scene 側へ保持する。
    float aspectRatio = 16.0f / 9.0f;
    float nearZ   = 0.1f;
    float farZ    = 1000.0f;
    bool  isMain  = true;
    bool  enabled = true;
    fbzz::LayerMask cullingMask = fbzz::Layer::Everything;

    const char* GetTypeName() const { return "Camera"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("isMain", isMain);
        r.Field("fovY", fovY);
        r.Field("aspectRatio", aspectRatio);
        r.Field("nearZ", nearZ);
        r.Field("farZ", farZ);
        r.Field("cullingMask", reinterpret_cast<int&>(cullingMask));
    }
};

} // namespace fbzz::scene
