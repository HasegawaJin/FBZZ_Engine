/// @file    EnvironmentResources.hpp
/// @brief   空連動 IBL のためにフレームをまたいで生存する GPU リソースと状態の入れ物。
/// @author  Hasegawa Jin
/// @date    2026-07-01

/// @note 「空 → 動的 IBL」の要石となる状態オブジェクト。空をキューブマップへ焼いた "SkyEnvCube" と、
/// @note それを畳み込んだ "SkyIrradiance"/"SkyPrefilter"、SkyLight として消費する方向光/色を保持する。
/// @note System にしない理由: XxxSystem はスケジューラが毎フレーム呼ぶステートレス関数で
/// @note フレームをまたぐ GPU リソースを持てないが、空連動 IBL はキャッシュと dirty 制御を
/// @note 要する永続状態のため、OcclusionCuller と同じく非 System ヘルパーとして持つ。
/// @see Docs/design/water-waves.md 空反射と雲の捕捉キャッシュ
#pragma once

#include <Graphics/Renderer/ResourceHandle.hpp>
#include <Graphics/Renderer/ProbeCaptureBudget.hpp>
#include <Graphics/Renderer/RenderEnvironment.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>

namespace fbzz::renderer {
class ResourceManager;

class EnvironmentResources {
public:
    ProbeCaptureBudget probeCaptureBudget;
    /// @note SkySignature — 空の見た目を決めるパラメータの要約。
    /// @note 密度・照明設定は変更時、雲の時刻・位置は間引いて比較し、毎ビューの畳み込みを防ぐ。
    struct SkySignature {
        /// @note 太陽の進行方向 (正規化, 真下=正午相当)
        math::Vector3 sunDirection     = {0.0f, -1.0f, 0.0f};
        /// @note Rayleigh 散乱係数
        math::Vector3 rayleigh         = math::Vector3::ZERO;
        float         mieScattering    = 0.0f;
        float         skyScatterIntensity = 0.0f;
        float         mieG             = 0.0f;
        float         planetRadius     = 0.0f;
        float         atmosphereRadius = 0.0f;
        math::Vector3 lightColor = math::Vector3::ONE;
        math::Vector3 ambientColor = math::Vector3::ZERO;
        float skyDimmer = 0.0f;
        uint64_t shaderVersion = 0;
        bool cloudEnabled = false;
        RenderCloudConstants cloud{};
        math::Vector3 capturePosition = math::Vector3::ZERO;

        /// @note 大気・照明の設定を比較する。太陽方向は ConsumeDirty の角度閾値で間引く。
        bool AtmosphereApproxEquals(const SkySignature& o, float eps = 1.0e-4f) const;
        /// @note 雲時刻だけを除き、主ビューと同じ密度・照明設定を比較する。
        bool CloudApproxEquals(const SkySignature& o, float eps = 1.0e-4f) const;
    };

    /// @name ベイク結果ハンドル (空連動)
    /// @{
    /// @note 空なら未生成。SkyCapture / SkyLightBake パスが dirty 時に書き込む。
    renderer::ResourceHandle<renderer::TextureTag> skyEnvCube;    ///< @note "SkyEnvCube"   : 空を焼いた環境キューブマップ
    renderer::ResourceHandle<renderer::TextureTag> skyIrradiance; ///< @note "SkyIrradiance": 拡散 irradiance キューブマップ
    renderer::ResourceHandle<renderer::TextureTag> skyPrefilter;  ///< @note "SkyPrefilter" : 鏡面 prefiltered キューブマップ
    uint32_t prefilteredMipCount = 0;                             ///< @note prefilter の mip 数 (maxMipLevel = この値 - 1)
    /// @note Successful convolution publishes fresh textures that are never written again before replacement.
    const renderer::ResourceManager* immutableIblOwner = nullptr;
    renderer::ResourceHandle<renderer::TextureTag> immutableIrradiance, immutablePrefilter;
    uint64_t immutableIblEpoch = 0;

    /// @note SkyCapture が空を焼き直したフレームに true を立て、SkyLightBake が畳み込み後に false に戻す。
    /// @note SkyCapture と SkyLightBake は別パスのため、「今フレーム焼いたから畳み込みも要る」を橋渡しする。
    bool needsConvolution = false;
    /// @}

    /// @name SkyLight として消費する方向光 (昼夜で変化)
    /// @{
    math::Vector3 skyLightDirection = {0.0f, -1.0f, 0.0f}; ///< @note 太陽光の進行方向
    math::Vector3 skyLightColor     = math::Vector3::ONE;  ///< @note 太陽光の色 × 強度

    /// @note ConsumeDirty — 空が意味のある変化をし、かつ最低ベイク間隔を満たしたら true を返す。
    /// @note 「dirty を消費する」セマンティクス: true を返したフレームで呼び出し側がベイクを実行する。
    /// @note 太陽方向は角度閾値 (約1.5°) で、時間全体は最低間隔 (約33ms) で間引き、時間帯アニメ中も
    /// @note 雲だけの変化は 0.5 秒で間引き、捕捉位置は前回から 50 m 以上離れた場合だけ変える。
    /// @param currentTimeSeconds 単調な実時間の経過秒。停止中の編集とカメラ移動も更新できること。
    /// @param frameStamp ResourceManager の描画フレーム。0 はフレーム単位の抑制を行わない。
    /// @note 全ビューで環境 1 個を共有し、雲の時間・捕捉位置だけの更新は最大 2 Hz、同一フレームは一度まで。
    bool ConsumeDirty(const SkySignature& current, float currentTimeSeconds, uint64_t frameStamp = 0);

    /// @note 解像度変更・テクスチャ破棄後に次の描画フレームで必ず再捕捉する。
    void MarkDirty() { m_forceDirty = true; }

    /// @note HasBakedTextures — IBL 消費側が空連動テクスチャを束縛できる状態か。
    bool HasBakedTextures() const { return skyIrradiance.IsValid() && skyPrefilter.IsValid(); }
    /// @}

private:
    SkySignature m_lastSignature;
    float        m_lastBakeTime = 0.0f;  ///< @note 最後に true を返した (= ベイクした) 時刻(秒)。最低間隔スロットル用
    bool         m_hasSignature = false; ///< @note 初回ベイク前は signature 未確定
    bool         m_forceDirty   = true;  ///< @note 初回は必ずベイクする
    uint64_t     m_lastCaptureFrame = 0;
};

} /// @note namespace fbzz::renderer
