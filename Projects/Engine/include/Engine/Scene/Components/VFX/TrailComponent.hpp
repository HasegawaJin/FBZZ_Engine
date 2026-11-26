/// @file    TrailComponent.hpp
/// @brief   移動体の軌跡をリボン状メッシュとして描画するための制御点・外観パラメータ。
/// @author  Hasegawa Jin
/// @date    2026-06-06
#pragma once
#include <Engine/Scene/Components/Rendering/TransientGpuState.hpp>

#include <Graphics/Effects/Trails/RenderTrailInput.hpp>
#include <Engine/Scene/VFX/ParticleCurve.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::scene {

using renderer::TrailPoint;
using renderer::TrailAlignment;
using renderer::TrailUVMode;
using renderer::TrailWidthEasing;

/// @brief GameObject に追従するトレイルの設定とランタイム状態。
/// @note System がリングバッファへ制御点を追加し、毎フレーム GPU 頂点バッファへリボンを展開する。
/// @note GPU 所有権と再確保用キャッシュは Component の値コピーへ含めない。
struct TrailComponentGpuState {
    /// @brief GPU リソースは保存対象ではない。帯の頂点は TrailRenderPass がビューごとにプールから借りる。
    /// @note Scene View と Game View で帯の形が違うため、Component に 1 本持たせると奪い合う。
    renderer::ResourceHandle<renderer::TextureTag> texture; ///< 材質から解決した帯のアルベドテクスチャ。
    renderer::OwnedResource<renderer::ConstantBufferTag> trailCB; ///< 帯の色・幅・UV などを描画に渡す定数バッファ。
    std::string loadedTexturePath; ///< 現在の帯のアルベドテクスチャを取得した参照パス。
    std::string loadedMaterialPath; ///< materialPath の変更検出用。シーン保存対象外。

};

struct TrailComponent : TransientGpuState<TrailComponentGpuState> {
    bool enabled = true; ///< リボン描画に向けて新しい軌跡サンプルを取得する。

    float duration       = 1.0f; ///< 軌跡のサンプルを保持してから消すまでの時間（秒）。
    int   maxPoints      = 64; ///< 軌跡に同時に保持する制御点数の上限。
    float sampleInterval = 1.0f / 30.0f; ///< 軌跡点を追加できる最小時間間隔（秒）。
    float minVertexDist  = 0.02f; ///< 既存の先端から点を追加するための最小移動距離（ワールド m）。

    float widthStart = 0.20f; ///< 軌跡の新しい先端の帯の幅（m）。
    float widthEnd   = 0.02f; ///< 軌跡の古い端の帯の幅（m）。幅カーブが無効なときに使う。

    /// @brief 1 フレームでこれ以上跳んだら点列を捨てて描き直す [ワールド]。0 で «切らない»。
    /// @note 既定 0 (opt-in) の理由: 瞬間移動をするかどうかは実体ごとに違うため、一律のしきい値だと
    /// @note 速い剣閃や乗り物の軌跡を誤って切ってしまう。手動 Clear() では、テレポートを書く側と
    /// @note トレイルを持つ GameObject (武器の子など) が別なことがあり呼び忘れが起きるため不十分。
    float breakDistance = 0.0f; ///< 軌跡を切って新しい帯にする 1 フレームの移動距離（ワールド m）。0 で無効。

    /// @brief 幅の多キー化。無効 (既定) なら widthStart/widthEnd + widthEasing の 2 点のまま。
    /// @note 新しいカーブ型を作らない理由: ParticleCurve は Inspector のカーブエディタ・.curve の
    /// @note 読み書き・TOML シリアライズが既に通っている。widthStart を倍率の基準として残すのは、
    /// @note カーブは形だけを持たせ実寸を 1 か所で決められるようにするため。
    bool widthCurveEnabled = false; ///< 帯の幅を widthCurve に従って変化させる。
    ParticleCurve widthCurve; ///< age に対する倍率。実寸 = widthStart × この値

    /// @brief 色の多キー化。無効 (既定) なら colorStart/colorEnd の 2 点のまま。時刻 0 が帯の
    /// @note 先端 (colorStart 側)、1 が消え際 (colorEnd 側)。
    /// @note 補間は GPU (Trail.hlsl) 側で行うため、キーの間はリニア空間で混ざる。
    /// @note ParticleGradient::colorSpace は帯では効かない (キーの色そのものは一致する)。
    bool colorGradientEnabled = false; ///< 帯の色を二点間の補間に代えて colorGradient から求める。
    ParticleGradient colorGradient; ///< 軌跡の新しい先端を 0、古い端を 1 とする帯の RGBA 色。
    bool beamMode = false; ///< Beam は移動履歴ではなくローカル2端点を毎フレーム固定リボンとして描く。
    math::Vector3 beamStart = math::Vector3::ZERO; ///< ビームの始点。beamWorldSpace でワールド、通常はオブジェクトローカル。
    math::Vector3 beamEnd = { 0.0f, 0.0f, 5.0f }; ///< ビームの終点。beamWorldSpace でワールド、通常はオブジェクトローカル。

    /// @brief beamStart / beamEnd の間を通す中間点。空なら 2 端点の直線。
    /// @note VFXBeamComponent が毎フレーム作り直すため、シーンへは保存しない
    /// @note (保存すると「止めた瞬間の形」がアセットに焼き付く)。
    std::vector<math::Vector3> beamPoints; ///< 直線の代わりにビームが通る内部の折れ点。空なら両端を直結する。

    /// @brief beamPoints / beamStart / beamEnd をワールド座標として解釈する。
    /// @note 2 つの実体を結ぶビームは、どちらか一方のローカル空間では表せない。
    bool beamWorldSpace = false; ///< ビームの端点と折れ点をワールド座標として解釈する。

    TrailWidthEasing widthEasing = TrailWidthEasing::Linear; ///< 二点で指定した帯の幅を補間する方式。幅カーブを使う場合は無視する。
    math::Vector4 colorStart = { 1.0f, 1.0f, 1.0f, 1.0f }; ///< 帯の新しい先端に使う RGBA 色。
    math::Vector4 colorEnd   = { 1.0f, 1.0f, 1.0f, 0.0f }; ///< 消える側の帯の端に使う RGBA 色。

    TrailAlignment alignment = TrailAlignment::CameraFacing; ///< リボン面をカメラなどに向ける際の姿勢合わせ方式。
    int smoothSubdivisions = 0; ///< 帯を滑らかにするためサンプル間に追加する補間分割数。
    /// @brief attachBone / attachOffset — SkinnedMeshRenderer のボーン GameObject にサンプル位置を追従させる。
    /// @note 武器の先端や手首など、GameObject 原点以外から Trail を発生させたいケースを Component 単体で扱う。
    std::string attachBone; ///< 軌跡点の取得に使う骨の名前。空ならオブジェクト自身。
    math::Vector3 attachOffset = math::Vector3::ZERO; ///< サンプリングする点の、追従先ローカルでの位置オフセット。
    /// @brief clearOnDisable — enabled=false 時に点列を即破棄するか、duration による自然消滅を待つか。
    /// @note ScriptTrailProxy::SetEnabled(false, false) で「記録だけ止めてフェードアウト」を選べるようにする。
    bool clearOnDisable = true; ///< 無効化時に保存済みの軌跡点を破棄する。false では寿命まで残す。

    /// @brief .mat アセットへの参照。albedo テクスチャを .mat から解決する。
    /// @note テクスチャを .mat に集約することで複数 Trail 間での共有と Editor ピッカーによるアセット管理を可能にする。
    std::string materialPath; ///< 帯に使うアルベドテクスチャなどを解決する .mat の参照。
    TrailUVMode uvMode = TrailUVMode::Stretch; ///< 帯の長手方向の UV を引き伸ばすか繰り返すかを選ぶ。
    float uvScrollSpeed = 0.0f; ///< 帯の長手方向の UV を時間で移動する速度（UV/秒）。
    float uvTiling      = 1.0f; ///< 帯の長手方向に設定する UV の繰り返し倍率。

    /// @brief 固定サイズリングバッファ。maxPoints 変更時は TrailRenderSystem が再初期化する。
    /// @note std::deque ではなく連続メモリにすることで、毎フレームの走査と頂点展開のキャッシュ効率を保つ。
    std::vector<TrailPoint> pointBuffer; ///< 軌跡の位置と時刻を保持する固定容量のリングバッファ。
    int   ringHead       = 0; ///< pointBuffer 内の最も古い有効な点の添字。
    int   ringTail       = 0; ///< pointBuffer 内で次に点を書き込む位置の添字。
    int   ringCount      = 0; ///< pointBuffer に記録した有効な点数。
    float lastSampleTime = -1.0f; ///< 前回の軌跡サンプルを取得した時刻（秒）。負値は未取得。
    uint64_t lastExtractionFrame = UINT64_MAX; ///< 履歴を最後に更新したリソースフレームの番号。UINT64_MAX は未抽出。

    const char* GetTypeName() const { return "Trail"; }

    /// @brief Inspector / Serializer から編集・保存する公開パラメータ。
    /// @note enum は IReflector が直接扱わないため int に変換し、無効値は CameraFacing / WorldUp にクランプする。
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

/// @brief 多キーの幅を使うか。
/// @note キーが 2 点に届かないカーブは形になっていないので、従来の widthStart / widthEnd へ落とす。
[[nodiscard]] inline bool TrailUsesWidthCurve(const TrailComponent& trail)
{
    return trail.widthCurveEnabled && trail.widthCurve.keyCount >= 2;
}

/// @brief 多キーの色を使うか。判断基準は幅と同じ。
[[nodiscard]] inline bool TrailUsesColorGradient(const TrailComponent& trail)
{
    return trail.colorGradientEnabled && trail.colorGradient.keyCount >= 2;
}

/// @brief 前のサンプル位置から今の位置への移動を瞬間移動と見なすか。
/// @return breakDistance <= 0 のときは常に false (切らない)。
[[nodiscard]] inline bool TrailIsDiscontinuous(const TrailComponent& trail,
                                               const math::Vector3& previous,
                                               const math::Vector3& current)
{
    if (trail.breakDistance <= 0.0f) {
        return false;
    }
    return (current - previous).LengthSq() > trail.breakDistance * trail.breakDistance;
}

/// @brief age (0 = 最古の点, 1 = 最新の点) に対する帯の幅 [ワールド]。
/// @param[in] easedAge widthEasing を掛けた age。カーブが有効なときは使わない
/// @note (カーブ自身が形を持っているため、二重に曲げない)。
[[nodiscard]] inline float TrailWidthAt(const TrailComponent& trail, float age, float easedAge)
{
    if (TrailUsesWidthCurve(trail)) {
        return trail.widthStart * trail.widthCurve.Evaluate(age);
    }
    return trail.widthEnd + (trail.widthStart - trail.widthEnd) * easedAge;
}

}
