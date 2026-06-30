// FBZZ Engine
// EnvironmentResources.hpp | fbzz::scene
// 空連動 IBL のためにフレームをまたいで生存する GPU リソースと状態の入れ物。
//
// 概要:
//   「空 → 動的 IBL」(環境システム設計ドキュメント Phase A) の要石となる状態オブジェクト。
//   空をキューブマップへ焼いた "SkyEnvCube" と、それを畳み込んだ "SkyIrradiance"/"SkyPrefilter"、
//   および SkyLight として消費する方向光/色を保持する。
//
// WHY (System ではない):
//   この engine の XxxSystem はスケジューラが毎フレーム呼ぶステートレス関数で、
//   フレームをまたぐ GPU リソースを持てない。一方、空連動 IBL は
//     1) キューブマップ/IBL テクスチャをフレーム間でキャッシュし
//     2) 太陽方向・大気パラメータが変化したフレームだけ再ベイクする
//   という永続状態と dirty 制御を要求する。そのため OcclusionCuller と同じく
//   「RenderPasses/ 内の System でないヘルパー (状態オブジェクト)」として実装し、
//   RenderSystem パイプラインの raw パス群がこれを RenderPassContext 経由で読み書きする。
//   (設計詳細: Docs/design/environment-system.md §5-1)
#pragma once

#include <Engine/Renderer/ResourceHandle.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>

namespace fbzz::scene {

class EnvironmentResources {
public:
    // SkySignature — 空の見た目を決めるパラメータの要約。
    // WHY: 毎フレーム IBL を焼き直すと高コストなため、この要約が変化したフレーム
    //      (太陽が動いた / 大気設定を変えた) だけ再ベイクするための dirty 判定に使う。
    struct SkySignature {
        math::Vector3 sunDirection     = {0.0f, -1.0f, 0.0f}; // 太陽の進行方向 (正規化, 真下=正午相当)
        math::Vector3 rayleigh         = math::Vector3::ZERO;  // Rayleigh 散乱係数
        float         mieScattering    = 0.0f;
        float         sunIntensity     = 0.0f;
        float         mieG             = 0.0f;
        float         planetRadius     = 0.0f;
        float         atmosphereRadius = 0.0f;

        // 各成分が eps 以内で一致するか。浮動小数のため厳密一致ではなく許容誤差で比較する。
        bool ApproxEquals(const SkySignature& o, float eps = 1.0e-4f) const;
    };

    // ── ベイク結果ハンドル (空連動) ──────────────────────────────────────────
    // 空なら未生成。SkyCapture / SkyLightBake パスが dirty 時に書き込む。
    renderer::ResourceHandle<renderer::TextureTag> skyEnvCube;    // "SkyEnvCube"   : 空を焼いた環境キューブマップ
    renderer::ResourceHandle<renderer::TextureTag> skyIrradiance; // "SkyIrradiance": 拡散 irradiance キューブマップ
    renderer::ResourceHandle<renderer::TextureTag> skyPrefilter;  // "SkyPrefilter" : 鏡面 prefiltered キューブマップ
    uint32_t prefilteredMipCount = 0;                             // prefilter の mip 数 (maxMipLevel = この値 - 1)

    // SkyCapture が空を焼き直したフレームに true を立て、SkyLightBake が畳み込み後に false に戻す。
    // WHY: SkyCapture と SkyLightBake は別パスのため、「今フレーム焼いたから畳み込みも要る」を橋渡しする。
    bool needsConvolution = false;

    // ── SkyLight として消費する方向光 (昼夜で変化) ───────────────────────────
    math::Vector3 skyLightDirection = {0.0f, -1.0f, 0.0f}; // 太陽光の進行方向
    math::Vector3 skyLightColor     = math::Vector3::ONE;  // 太陽光の色 × 強度

    // ConsumeDirty — current が前回と異なる (または強制 dirty) なら true を返し、内部状態を更新する。
    // WHY: 「dirty を消費する」セマンティクス。true を返したフレームで呼び出し側がベイクを実行し、
    //      次フレーム以降は同一 signature ならスキップされる (キャッシュ)。
    bool ConsumeDirty(const SkySignature& current);

    // MarkDirty — 次回の ConsumeDirty を必ず true にする (解像度変更・テクスチャ破棄後の強制再生成用)。
    void MarkDirty() { m_forceDirty = true; }

    // HasBakedTextures — IBL 消費側が空連動テクスチャを束縛できる状態か。
    bool HasBakedTextures() const { return skyIrradiance.IsValid() && skyPrefilter.IsValid(); }

private:
    SkySignature m_lastSignature;
    bool         m_hasSignature = false; // 初回ベイク前は signature 未確定
    bool         m_forceDirty   = true;  // 初回は必ずベイクする
};

} // namespace fbzz::scene
