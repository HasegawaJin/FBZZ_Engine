/// @file    ReflectionProbeComponent.hpp
/// @brief   局所的な環境反射を静的または実行時キューブマップで提供するプローブコンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @note EnvironmentLightComponent はシーン全体のグローバル IBL を管理するが、室内や窓際など
/// @note 局所的に異なる反射環境が必要な場所には別のキューブマップが要る。
#pragma once
#include <Engine/Scene/Components/Rendering/TransientGpuState.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <cstdint>
#include <string>
namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

/// @brief 動的プローブに含める反射情報の範囲。
/// @note 空だけの更新は昼夜変化へ追従し、完全方式は室内や動く配置物の反射を得る。
/// @note 完全方式は 6 面分のジオメトリ描画を要するため、用途ごとに明示選択する。
enum class ReflectionProbeCaptureMode : uint8_t {
    Static = 0,
    DynamicSky,
    DynamicScene
};

/// @note GPU 所有権と再確保用キャッシュは Component の値コピーへ含めない。
struct ReflectionProbeComponentGpuState {
    /// @note ランタイム GPU リソース。Scene / Prefab には保存しない。
    /// @note 動的プローブは Scene の瞬間値であり、GPU ハンドルを Scene / Prefab へ保存しない。
    /// @note RenderSystem が解像度変更時に再生成し、更新時に畳み込み結果を差し替える。
    renderer::OwnedResource<renderer::RenderTargetTag> runtimeCubeRT;
    renderer::OwnedResource<renderer::TextureTag> runtimeIrradiance;
    renderer::OwnedResource<renderer::TextureTag> runtimePrefilter;
    uint32_t runtimeResolution = 0;
    uint32_t runtimePrefilterMipCount = 0;
    const renderer::ResourceManager* runtimePublicationOwner = nullptr; ///< キャプチャ成功時に、以後書き換えない新規テクスチャを公開する。無効化は設定値を変えずにキャプチャ済みの証跡を消す。
    renderer::ResourceHandle<renderer::TextureTag> runtimePublishedIrradiance, runtimePublishedPrefilter;
    uint64_t runtimePublicationEpoch = 0;
    bool runtimeImmutablePublished = false;
    float    lastCaptureTime = -1.0e30f;

};

struct ReflectionProbeComponent : TransientGpuState<ReflectionProbeComponentGpuState> {
    bool          enabled         = true;
    std::string   cubemapPath; ///< 静的環境キューブマップ (.dds)
    ReflectionProbeCaptureMode captureMode = ReflectionProbeCaptureMode::Static;
    uint32_t      captureResolution = 128; ///< 動的キューブマップの 1 面解像度 [px]
    float         updateInterval = 1.0f; ///< 動的キャプチャの最短更新間隔 [s]。0 なら毎フレーム
    bool          refreshRequested = true; ///< 次フレームの更新要求
    float         influenceRadius = 5.0f; ///< 球影響半径 [m]
    float         intensity       = 1.0f; ///< 反射強度スケール
    bool          boxInfluence    = false; ///< true のときボックス影響範囲
    math::Vector3 boxExtents      = { 1.0f, 1.0f, 1.0f }; ///< ボックス半径 [m]

    const char* GetTypeName() const { return "ReflectionProbe"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled",         enabled);
        r.Field("cubemapPath",     cubemapPath);
        int captureModeValue = static_cast<int>(captureMode);
        r.Field("captureMode",     captureModeValue);
        captureMode = static_cast<ReflectionProbeCaptureMode>(captureModeValue);
        int captureResolutionValue = static_cast<int>(captureResolution);
        r.Field("captureResolution", captureResolutionValue);
        captureResolution = static_cast<uint32_t>(captureResolutionValue);
        r.Field("updateInterval",  updateInterval);
        r.Field("influenceRadius", influenceRadius);
        r.Field("intensity",       intensity);
        r.Field("boxInfluence",    boxInfluence);
        r.Field("boxExtents",      boxExtents);
    }
};

}
