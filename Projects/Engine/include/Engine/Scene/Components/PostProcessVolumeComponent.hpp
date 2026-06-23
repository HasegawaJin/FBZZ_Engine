// FBZZ Engine
// PostProcessVolumeComponent.hpp | fbzz::scene
// カメラ GameObject に付けてポストプロセス設定をシーン Inspector から制御するコンポーネント。
// WHY: ProjectSettings のポストプロセス設定をシーン単位で上書きすることで、
//      屋外・屋内・カットシーンなどカメラごとに異なる演出を簡単に切り替えられるようにする。
//      isGlobal=true (デフォルト) のときカメラに常時適用。
//      isGlobal=false のときカメラが influenceRadius 内に入った時点で適用される (予定)。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Engine/Renderer/RenderSettings.hpp>

namespace fbzz::scene {

struct PostProcessVolumeComponent {
    bool        enabled         = true;
    bool        isGlobal        = true;    // true: カメラに常時適用 / false: 範囲トリガー(将来対応)
    float       blendWeight     = 1.0f;   // 0=ProjectSettings, 1=このコンポーネントの設定
    float       influenceRadius = 10.0f;  // isGlobal=false 時の球影響半径 [m]

    // PostProcess 設定本体。DrawPostProcessInspector で直接編集される。
    renderer::PostProcessSettings settings;

    const char* GetTypeName() const { return "PostProcessVolume"; }

    // WHY: PostProcessSettings は 30+ フィールドを持つため、
    //      Reflect で個別列挙せず SceneSerializer で直接 TOML 化する。
    //      IReflector では基本フィールドのみ公開し、設定詳細は serialize 専用パスで扱う。
    void Reflect(IReflector& r)
    {
        r.Field("enabled",         enabled);
        r.Field("isGlobal",        isGlobal);
        r.Field("blendWeight",     blendWeight);
        r.Field("influenceRadius", influenceRadius);
    }
};

} // namespace fbzz::scene
