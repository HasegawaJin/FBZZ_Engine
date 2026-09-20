/// @file    EnvironmentResources.hpp
/// @brief   空連動 IBL のためにフレームをまたいで生存する GPU リソースと状態の入れ物。
/// @author  Hasegawa Jin
/// @date    2026-07-01

/// @note 「空 → 動的 IBL」の要石となる状態オブジェクト。空をキューブマップへ焼いた "SkyEnvCube" と、
/// @note それを畳み込んだ "SkyIrradiance"/"SkyPrefilter"、SkyLight として消費する方向光/色を保持する。
/// @note System にしない理由: XxxSystem はスケジューラが毎フレーム呼ぶステートレス関数で
/// @note フレームをまたぐ GPU リソースを持てないが、空連動 IBL はキャッシュと dirty 制御を
/// @note 要する永続状態のため、OcclusionCuller と同じく非 System ヘルパーとして持つ。
/// @see Docs/design/environment-system.md §5-1
#pragma once

#include <Graphics/Renderer/ResourceHandle.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>

namespace fbzz::renderer {

class EnvironmentResources {
public:
    /// @note SkySignature — 空の見た目を決めるパラメータの要約。
    /// @note 毎フレーム IBL を焼き直すと高コストなため、この要約が変化したフレーム (太陽が動いた/
    /// @note 大気設定を変えた) だけ再ベイクする dirty 判定に使う。
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

        /// @note 太陽方向を除く各成分が eps 以内で一致するか (浮動小数のため許容誤差で比較)。太陽方向は
        /// @note 時間帯アニメで連続的に動くためここには含めず、ConsumeDirty 側で角度閾値により間引く。
        /// @note 大気パラメータ等はユーザー編集による離散変化なので eps 比較で十分。
        bool AtmosphereApproxEquals(const SkySignature& o, float eps = 1.0e-4f) const;
    };

    /// @name ベイク結果ハンドル (空連動)
    /// @{
    /// @note 空なら未生成。SkyCapture / SkyLightBake パスが dirty 時に書き込む。
    renderer::ResourceHandle<renderer::TextureTag> skyEnvCube;    ///< @note "SkyEnvCube"   : 空を焼いた環境キューブマップ
    renderer::ResourceHandle<renderer::TextureTag> skyIrradiance; ///< @note "SkyIrradiance": 拡散 irradiance キューブマップ
    renderer::ResourceHandle<renderer::TextureTag> skyPrefilter;  ///< @note "SkyPrefilter" : 鏡面 prefiltered キューブマップ
    uint32_t prefilteredMipCount = 0;                             ///< @note prefilter の mip 数 (maxMipLevel = この値 - 1)

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
    /// @note IBL 畳み込みを毎フレーム走らせない。拡散/低roughness IBL は数フレーム遅延しても知覚
    /// @note できないため、GPU コスト (テクセル1万サンプルの畳み込み) を大きく削減できる。
    /// @param currentTimeSeconds Time::time (累積秒) を渡す。
    bool ConsumeDirty(const SkySignature& current, float currentTimeSeconds);

    /// @note MarkDirty — 次回の ConsumeDirty を必ず true にする (解像度変更・テクスチャ破棄後の強制再生成用)。
    void MarkDirty() { m_forceDirty = true; }

    /// @note HasBakedTextures — IBL 消費側が空連動テクスチャを束縛できる状態か。
    bool HasBakedTextures() const { return skyIrradiance.IsValid() && skyPrefilter.IsValid(); }
    /// @}

private:
    SkySignature m_lastSignature;
    float        m_lastBakeTime = 0.0f;  ///< @note 最後に true を返した (= ベイクした) 時刻(秒)。最低間隔スロットル用
    bool         m_hasSignature = false; ///< @note 初回ベイク前は signature 未確定
    bool         m_forceDirty   = true;  ///< @note 初回は必ずベイクする
};

} /// @note namespace fbzz::renderer
