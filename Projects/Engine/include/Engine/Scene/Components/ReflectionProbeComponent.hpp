/// @file    ReflectionProbeComponent.hpp
/// @brief   局所的な環境反射を静的または実行時キューブマップで提供するプローブコンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// @note EnvironmentLightComponent はシーン全体のグローバル IBL を管理するが、室内や窓際など
///       局所的に異なる反射環境が必要な場所には別のキューブマップが要る。動的キャプチャは
///       空だけを焼く軽量方式と、周辺メッシュも焼く完全方式を選べる。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <string>

namespace fbzz::scene {

/// ReflectionProbeCaptureMode — 動的プローブに含める反射情報の範囲。
/// @note 空だけの更新は昼夜変化へ低コストで追従でき、完全方式は室内や動く配置物の反射を
///       得られる代わりに 6 面分のジオメトリ描画を要するため、用途ごとに明示選択する。
enum class ReflectionProbeCaptureMode : uint8_t {
    Static = 0,
    DynamicSky,
    DynamicScene
};

struct ReflectionProbeComponent {
    bool          enabled         = true;
    std::string   cubemapPath;               ///< 静的環境キューブマップ (.dds)
    ReflectionProbeCaptureMode captureMode = ReflectionProbeCaptureMode::Static;
    uint32_t      captureResolution = 128;   ///< 動的キューブマップの 1 面解像度 [px]
    float         updateInterval = 1.0f;     ///< 動的キャプチャの最短更新間隔 [s]。0 なら毎フレーム
    bool          refreshRequested = true;   ///< Inspector / Script が次フレームの更新を要求するフラグ
    float         influenceRadius = 5.0f;    ///< 球影響半径 [m]。カメラがこの範囲内に入ると適用される
    float         intensity       = 1.0f;    ///< 反射強度スケール
    bool          boxInfluence    = false;   ///< true のとき球ではなくボックス形状で影響範囲を定義
    math::Vector3 boxExtents      = { 1.0f, 1.0f, 1.0f }; ///< ボックス半径 [m] (boxInfluence=true 時のみ使用)

    /// ランタイム GPU リソース。Scene / Prefab には保存しない。
    /// @note 動的プローブはアセットではなく現在の Scene の瞬間値であり、保存すると古い GPU
    ///       ハンドルを復元してしまう。RenderSystem が解像度変更時に再生成し、更新時に
    ///       畳み込み結果を差し替える。
    renderer::ResourceHandle<renderer::RenderTargetTag> runtimeCubeRT;
    renderer::ResourceHandle<renderer::TextureTag> runtimeIrradiance;
    renderer::ResourceHandle<renderer::TextureTag> runtimePrefilter;
    uint32_t runtimeResolution = 0;
    uint32_t runtimePrefilterMipCount = 0;
    float    lastCaptureTime = -1.0e30f;

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

} // namespace fbzz::scene
