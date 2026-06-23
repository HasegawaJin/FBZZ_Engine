// FBZZ Engine
// EnvironmentLightComponent.hpp | fbzz::scene
// シーン全体の IBL (Image-Based Lighting) 設定を GameObject として管理するコンポーネント。
// WHY: ProjectSettings の ibl 設定はグローバルだが、このコンポーネントをシーンに置くことで
//      Scene Inspector から IBL を差し替えられ、シーンごとに異なる環境光を使い分けられる。
//      RenderSystem が最初のアクティブなコンポーネントを採用し ProjectSettings を上書きする。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <string>

namespace fbzz::scene {

// EnvironmentLightComponent — シーン単位のグローバル IBL 設定コンポーネント。
// Unreal の SkyLight に相当。シーン内に1つ配置する。
struct EnvironmentLightComponent {
    bool        enabled       = true;
    std::string irradiancePath;           // 拡散 IBL irradiance cubemap (.dds)
    std::string prefilterPath;            // 鏡面 IBL prefiltered cubemap (.dds)
    float       intensity     = 1.0f;     // 環境光全体スケール
    float       diffuseScale  = 1.0f;     // 拡散成分スケール
    float       specularScale = 1.0f;     // 鏡面成分スケール
    int         maxMipLevel   = 4;        // prefilter の最大 mip インデックス (bake mip 数 - 1)

    const char* GetTypeName() const { return "EnvironmentLight"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled",        enabled);
        r.Field("irradiancePath", irradiancePath);
        r.Field("prefilterPath",  prefilterPath);
        r.Field("intensity",      intensity);
        r.Field("diffuseScale",   diffuseScale);
        r.Field("specularScale",  specularScale);
        r.Field("maxMipLevel",    maxMipLevel);
    }
};

} // namespace fbzz::scene
