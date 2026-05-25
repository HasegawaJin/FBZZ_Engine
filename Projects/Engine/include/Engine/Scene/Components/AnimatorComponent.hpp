// FBZZ Engine
// AnimatorComponent.hpp | fbzz::scene
// スケルタルアニメーション再生状態コンポーネント
// Model の AnimationClip を参照し、現在時刻や再生速度を保持する。
// 骨行列の計算と GPU 転送は AnimatorSystem が行う。
#pragma once
#include <Engine/Asset/AnimationClip.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Matrix4.hpp>
#include <string>
#include <vector>

namespace fbzz::scene {

struct AnimatorComponent {
    bool enabled = true;
    int clipIndex = 0;
    std::string clipName;
    float time = 0.0f;
    float speed = 1.0f;
    bool loop = true;
    bool playing = true;

    // アニメーションクリップを含む FBX ファイルパス。シリアライズ対象
    std::vector<std::string> clipSources;

    // ランタイム専用。初回更新時に clipSources から再構築する
    std::vector<asset::AnimationClip> clips;
    bool clipsLoaded = false;

    std::vector<math::Matrix4> boneMatrices;
    std::vector<math::Matrix4> nodeGlobalTransforms;
    renderer::ResourceHandle<renderer::ConstantBufferTag> skinningBuffer;

    const char* GetTypeName() const { return "Animator"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",   enabled);
        r.Field("clipName",  clipName);
        r.Field("clipIndex", clipIndex);
        r.Field("time",      time);
        r.Field("speed",     speed);
        r.Field("loop",      loop);
        r.Field("playing",   playing);
        // clipSources は Reflect では扱わない (vector<string> 非対応のため SceneSerializer で直接処理)
    }
};

} // namespace fbzz::scene
