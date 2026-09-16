/// @file    TrailComponent.hpp
/// @brief   移動体の軌跡をリボン状メッシュとして描画するための制御点・外観パラメータ。
/// @author  Hasegawa Jin
/// @date    2026-06-06
#pragma once

#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/ParticleCurve.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::scene {

// TrailPoint — トレイルを構成する制御点 1 個分のワールド座標と生成時刻。
// WHY: 点列だけを保存し、リボン幅・色・UV は描画時に再計算することでパラメータ変更を即時反映する。
struct TrailPoint {
    math::Vector3 position = math::Vector3::ZERO;
    float         timestamp = 0.0f;
};

// TrailAlignment — リボン断面をどの基準方向に向けるかを表す。
// CameraFacing は剣閃など常に見やすいエフェクト、WorldUp はタイヤ跡など地面基準の帯に使う。
enum class TrailAlignment : uint8_t {
    CameraFacing = 0,
    WorldUp      = 1,
};

// TrailUVMode — U 座標をトレイル全体へ正規化するか、ワールド長でタイルするかを選ぶ。
// Stretch は剣閃の一枚絵、Tile は長い軌跡へ繰り返し模様を流す用途に使う。
enum class TrailUVMode : uint8_t {
    Stretch = 0,
    Tile    = 1,
};

// TrailWidthEasing — 古い点から新しい点へ幅を補間するときの曲線。
// WHY: 線形だけでは先端だけ鋭く細る軌跡や、根元を長く太く残す演出を作りにくい。
enum class TrailWidthEasing : uint8_t {
    Linear    = 0,
    EaseIn    = 1,
    EaseOut   = 2,
    EaseInOut = 3,
};

// TrailComponent — GameObject に追従するトレイルの設定とランタイム状態。
// WHAT: System がリングバッファへ制御点を追加し、毎フレーム GPU 頂点バッファへリボンを展開する。
struct TrailComponent {
    bool enabled = true;

    float duration       = 1.0f;
    int   maxPoints      = 64;
    float sampleInterval = 1.0f / 30.0f;
    float minVertexDist  = 0.02f;

    float widthStart = 0.20f;
    float widthEnd   = 0.02f;

    // 1 フレームでこれ以上跳んだら点列を捨てて描き直す [ワールド]。0 で «切らない»。
    //
    // WHY 既定を 0 にするか: 閾値を «それらしい値» で入れると、今あるシーンの
    //     速い剣閃や乗り物の軌跡が «たまに途切れる» ようになる。瞬間移動を
    //     するのは作った本人が知っている実体だけなので、opt-in にする。
    // WHY 手動 Clear() では足りないか: テレポートは «位置を書いた側» が知っていても、
    //     トレイルを持つのは武器の子 GameObject だったりする。1 本の長い筋は
    //     «誰が Clear を呼び忘れたか» を探す不具合になっていた。
    float breakDistance = 0.0f;

    // 幅の多キー化。無効 (既定) なら widthStart/widthEnd + widthEasing の 2 点のまま。
    //
    // WHY 新しいカーブ型を作らないか: ParticleCurve は Inspector のカーブエディタ・
    //     .curve の読み書き・TOML シリアライズが既に通っている。帯のためだけに
    //     もう 1 種類の «キーと補間» を持つと、editor 側も倍になる。
    // WHY widthStart を残して倍率にするか: カーブは形だけを持たせ、実寸は
    //     1 か所 (widthStart) で決められるようにする。太さの微調整でキーを
    //     全部触り直すことにならない。
    bool widthCurveEnabled = false;
    ParticleCurve widthCurve; // age に対する倍率。実寸 = widthStart × この値

    // 色の多キー化。無効 (既定) なら colorStart/colorEnd の 2 点のまま。
    // 時刻 0 は帯の «先端» (colorStart 側)、1 が消え際 (colorEnd 側)。
    //
    // NOTE: 補間は GPU (Trail.hlsl) 側で行うため、キーの間はリニア空間で混ざる。
    //       ParticleGradient::colorSpace は帯では効かない (キーの色そのものは一致する)。
    bool colorGradientEnabled = false;
    ParticleGradient colorGradient;
    // Beamは移動履歴ではなくローカル2端点を毎フレーム固定リボンとして描く。
    bool beamMode = false;
    math::Vector3 beamStart = math::Vector3::ZERO;
    math::Vector3 beamEnd = { 0.0f, 0.0f, 5.0f };

    // beamStart / beamEnd の間を通す中間点。空なら 2 端点の直線。
    // 書き手は VFXBeamComponent で毎フレーム作り直すため、シーンへは保存しない
    // (保存すると「止めた瞬間の形」がアセットに焼き付く)。
    std::vector<math::Vector3> beamPoints;

    // beamPoints / beamStart / beamEnd をワールド座標として解釈する。
    // 2 つの実体を結ぶビームは、どちらか一方のローカル空間では表せない。
    bool beamWorldSpace = false;

    TrailWidthEasing widthEasing = TrailWidthEasing::Linear;
    math::Vector4 colorStart = { 1.0f, 1.0f, 1.0f, 1.0f };
    math::Vector4 colorEnd   = { 1.0f, 1.0f, 1.0f, 0.0f };

    TrailAlignment alignment = TrailAlignment::CameraFacing;
    int smoothSubdivisions = 0;
    // attachBone / attachOffset — SkinnedMeshRenderer のボーン GameObject にサンプル位置を追従させる。
    // WHY: 武器の先端や手首など、GameObject 原点以外から Trail を発生させたいケースを Component 単体で扱う。
    std::string attachBone;
    math::Vector3 attachOffset = math::Vector3::ZERO;
    // clearOnDisable — enabled=false 時に点列を即破棄するか、duration による自然消滅を待つか。
    // WHY: ScriptTrailProxy::SetEnabled(false, false) で「記録だけ止めてフェードアウト」を選べるようにする。
    bool clearOnDisable = true;

    // .mat アセットへの参照。albedo テクスチャを .mat から解決する。
    // WHY: テクスチャを .mat に集約することで複数 Trail 間での共有と Editor ピッカーによるアセット管理を可能にする。
    std::string materialPath;
    TrailUVMode uvMode = TrailUVMode::Stretch;
    float uvScrollSpeed = 0.0f;
    float uvTiling      = 1.0f;

    // 固定サイズリングバッファ。maxPoints 変更時は TrailRenderSystem が再初期化する。
    // WHY: std::deque ではなく連続メモリにすることで、毎フレームの走査と頂点展開のキャッシュ効率を保つ。
    std::vector<TrailPoint> pointBuffer;
    int   ringHead       = 0;
    int   ringTail       = 0;
    int   ringCount      = 0;
    float lastSampleTime = -1.0f;

    // GPU リソースは保存対象ではない。帯の頂点は TrailRenderPass がビューごとにプールから借りる
    // (Scene View と Game View で帯の形が違うため、Component に 1 本持たせると奪い合う)。
    renderer::ResourceHandle<renderer::TextureTag> texture;
    renderer::ResourceHandle<renderer::ConstantBufferTag> trailCB;
    std::string loadedTexturePath;
    std::string loadedMaterialPath; // materialPath の変更検出用。シーン保存対象外。

    const char* GetTypeName() const { return "Trail"; }

    // Reflect — Inspector / Serializer から編集・保存する公開パラメータ。
    // WHY: enum は IReflector が直接扱わないため int に変換し、無効値は CameraFacing / WorldUp にクランプする。
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("duration", duration);
        r.Field("maxPoints", maxPoints);
        r.Field("sampleInterval", sampleInterval);
        r.Field("minVertexDist", minVertexDist);
        r.Field("widthStart", widthStart);
        r.Field("widthEnd", widthEnd);
        r.Field("breakDistance", breakDistance);
        r.Field("widthCurveEnabled", widthCurveEnabled);
        r.Field("widthCurve", widthCurve);
        r.Field("colorGradientEnabled", colorGradientEnabled);
        r.Field("colorGradient", colorGradient);
        r.Field("beamMode", beamMode);
        r.Field("beamStart", beamStart);
        r.Field("beamEnd", beamEnd);
        int widthEasingValue = static_cast<int>(widthEasing);
        r.Field("widthEasing", widthEasingValue);
        widthEasingValue = widthEasingValue < 0 ? 0 : (widthEasingValue > 3 ? 3 : widthEasingValue);
        widthEasing = static_cast<TrailWidthEasing>(widthEasingValue);
        r.ColorField("colorStart", colorStart);
        r.ColorField("colorEnd", colorEnd);

        int alignmentValue = static_cast<int>(alignment);
        r.Field("alignment", alignmentValue);
        alignmentValue = alignmentValue < 0 ? 0 : (alignmentValue > 1 ? 1 : alignmentValue);
        alignment = static_cast<TrailAlignment>(alignmentValue);

        r.Field("smoothSubdivisions", smoothSubdivisions);
        r.Field("attachBone", attachBone);
        r.Field("attachOffset", attachOffset);
        r.Field("clearOnDisable", clearOnDisable);
        r.Field("materialPath", materialPath);
        int uvModeValue = static_cast<int>(uvMode);
        r.Field("uvMode", uvModeValue);
        uvModeValue = uvModeValue < 0 ? 0 : (uvModeValue > 1 ? 1 : uvModeValue);
        uvMode = static_cast<TrailUVMode>(uvModeValue);
        r.Field("uvScrollSpeed", uvScrollSpeed);
        r.Field("uvTiling", uvTiling);
    }
};

/// 多キーの幅を使うか。キーが 2 点に届かないカーブは «形になっていない» ので
/// 従来の widthStart / widthEnd へ落とす。
[[nodiscard]] inline bool TrailUsesWidthCurve(const TrailComponent& trail)
{
    return trail.widthCurveEnabled && trail.widthCurve.keyCount >= 2;
}

/// 多キーの色を使うか。判断基準は幅と同じ。
[[nodiscard]] inline bool TrailUsesColorGradient(const TrailComponent& trail)
{
    return trail.colorGradientEnabled && trail.colorGradient.keyCount >= 2;
}

/// 前のサンプル位置から今の位置への移動を «瞬間移動» と見なすか。
/// breakDistance <= 0 のときは常に false (切らない)。
[[nodiscard]] inline bool TrailIsDiscontinuous(const TrailComponent& trail,
                                               const math::Vector3& previous,
                                               const math::Vector3& current)
{
    if (trail.breakDistance <= 0.0f) return false;
    return (current - previous).LengthSq() > trail.breakDistance * trail.breakDistance;
}

/// age (0 = 最古の点, 1 = 最新の点) に対する帯の幅 [ワールド]。
/// @param easedAge widthEasing を掛けた age。カーブが有効なときは使わない
///                 (カーブ自身が形を持っているため、二重に曲げない)
[[nodiscard]] inline float TrailWidthAt(const TrailComponent& trail, float age, float easedAge)
{
    if (TrailUsesWidthCurve(trail))
        return trail.widthStart * trail.widthCurve.Evaluate(age);
    return trail.widthEnd + (trail.widthStart - trail.widthEnd) * easedAge;
}

} // namespace fbzz::scene
