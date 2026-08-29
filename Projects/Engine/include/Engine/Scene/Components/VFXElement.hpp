/// @file    VFXElement.hpp
/// @brief   VFX の生存窓 (VFXElement) と、そこから実コンポーネントを動かすエンベロープ群。
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY 生存窓を別コンポーネントにするか:
///   時間の正本は 1 オブジェクトにつき 1 箇所、という規約でエフェクトを組む。
///   ParticleEmitter (startDelay/duration/loop) ・TrailComponent (duration) ・
///   DecalComponent (lifetime/fadeTime) は自前で時間を持つので、VFXElement を付けない。
///   時間を持たない LightComponent / ParticleForceField / MeshRenderer /
///   WindZoneComponent / VFXScreenEffect 系にだけ付けて、窓と重みを与える。
///   両方に duration があると「どちらが効くのか」が読めなくなる。
///
/// WHY エンベロープを VFX 専用にしないか:
///   「時間に沿って明るさを落とす」「膨らませる」は VFX の外でも要る。
///   VFXElement が無いオブジェクトでも、DecalComponent の age やルートの時刻から
///   進捗を解決するため (VFXSystem::ResolveProgress)、ふつうのシーンでも使える。
#pragma once
#include <Engine/Scene/ParticleCurve.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <string>

namespace fbzz::scene {

/// VFX ルートの時刻に対する、この GameObject の生存窓。
///
/// 窓の中だけ GameObject が active になり、weight に weightCurve の値が入る。
/// 旧 .vfx の startOffset / duration と、ScreenEffect の fadeIn/fadeOut ・
/// CameraShake の falloffPower ・TimeScale の blendIn/blendOut を 1 本のカーブへ畳んだもの。
struct VFXElement {
    bool enabled = true;

    /// VFX 先頭からの遅延 [秒]。
    float startDelay = 0.0f;
    /// 窓の長さ [秒]。0 以下ならルートが終わるまで開いたままにする。
    float duration = 1.0f;
    /// 窓の終わりで頭出しして繰り返す。
    bool loop = false;

    /// 窓の進捗 0..1 に対する重み。既定は矩形 (常に 1)。
    /// VFXScreenEffect / VFXCameraShake / VFXTimeScale の weight へそのまま入り、
    /// エンベロープ各種はこれではなく進捗そのものを見る。
    ParticleCurve weightCurve{
        {{ {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f},
           {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f} }},
        2, ParticleCurveInterpolation::Linear };

    /// 空でなければ時間ではなく VFXComponent::Trigger(name) で開始する。
    /// 旧 .vfx の OnCollision / OnDeath / OnAnimationEvent / OnTrigger の置き換え。
    std::string trigger;

    // --- ランタイム ---

    /// 窓の進捗 [0,1]。窓の外では 0 (開始前) または 1 (終了後)。
    float progress = 0.0f;
    /// weightCurve.Evaluate(progress)。窓の外では 0。
    float weight = 0.0f;
    /// trigger 待ちが解けた時刻 [秒]。負なら未発火。
    float triggeredAt = -1.0f;
    /// 前フレームの窓内時刻。loop の折り返しを検出して頭出しするために持つ。
    /// WHY 進捗の比較で足りないか: ループする窓は active のままなので、
    ///     SetActive の切り替わりを頼りにすると一度も頭出しされない。
    float lastLocalTime = -1.0f;

    const char* GetTypeName() const { return "VFX Element"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("startDelay", startDelay);
        r.Field("duration", duration);
        r.Field("loop", loop);
        r.Field("weightCurve", weightCurve);
        r.Field("trigger", trigger);
    }
};

/// 時間に沿った閃光。LightComponent の intensity / color を書き換える。
///
/// 基準値は Inspector に置かれた LightComponent の値そのもの (= ピーク) で、
/// 初回適用時に捕まえてから毎フレーム倍率を掛け直す。
/// WHY 基準値をここに複製しないか: LightComponent.intensity と二重管理になり、
///     「Inspector で明るくしたのに変わらない」という形で必ず食い違う。
struct VFXLightEnvelope {
    bool enabled = true;

    /// intensity への倍率。0..1 想定だが 1 超で増幅もできる。
    bool useIntensityCurve = true;
    ParticleCurve intensityCurve{
        {{ {0.0f, 1.0f}, {0.12f, 0.55f}, {1.0f, 0.0f}, {1.0f, 0.0f},
           {1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f} }},
        3, ParticleCurveInterpolation::Linear };

    /// 色の推移。alpha は明るさ倍率として rgb へ乗算する
    /// (白熱 → 橙 → 暗赤 のような色温度変化を 1 本で作れるようにするため)。
    bool useColorGradient = false;
    ParticleGradient colorGradient;

    // --- ランタイム ---
    float         baseIntensity = 0.0f;
    math::Vector3 baseColor     = { 1.0f, 1.0f, 1.0f };
    bool          captured      = false;

    const char* GetTypeName() const { return "VFX Light Envelope"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("useIntensityCurve", useIntensityCurve);
        r.Field("intensityCurve", intensityCurve);
        r.Field("useColorGradient", useColorGradient);
        r.Field("colorGradient", colorGradient);
    }
};

/// 時間に沿った膨張・収縮。localScale へ倍率を掛ける。
/// 衝撃波シェル・斬撃の板ポリ・魔法陣の立ち上がりがこれ 1 つで作れる。
struct VFXTransformEnvelope {
    bool enabled = true;

    /// localScale への倍率。既定は 0.1 倍から 4 倍へ、頭で一気に開いて減速する形。
    /// WHY 既定を線形にしないか: 線形だと «風船が膨らむ» 動きになり、衝撃波に見えない。
    bool useScaleCurve = true;
    ParticleCurve scaleCurve{
        {{ {0.0f, 0.1f}, {0.35f, 2.8f}, {1.0f, 4.0f}, {1.0f, 4.0f},
           {1.0f, 4.0f}, {1.0f, 4.0f}, {1.0f, 4.0f}, {1.0f, 4.0f} }},
        3, ParticleCurveInterpolation::Smooth };

    // --- ランタイム ---
    math::Vector3 baseScale = math::Vector3::ONE;
    bool          captured  = false;

    const char* GetTypeName() const { return "VFX Transform Envelope"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("useScaleCurve", useScaleCurve);
        r.Field("scaleCurve", scaleCurve);
    }
};

/// Mesh シェルのマテリアル未割当時に使う既定 .mat。
/// 加算・両面の Unlit なので、置いただけで衝撃波シェルとして成立する。
/// WHY 共通の Fallback.mat に落とさないか: 不透明マゼンタは «壊れている» 表示で、
///     エフェクトとしては使い物にならない。
inline constexpr const char* kVFXMeshFallbackMaterial =
    "Assets/Materials/Fallback/VFXMeshFallback.mat";

/// 時間に沿ったマテリアル値の書き換え。MaterialComponent::paramOverrides へ書く。
///
/// WHY 既定の書き先が "albedo" か: どのマテリアルにも必ずある共通パラメーターなので、
///     .mat を差し替えても色とフェードが黙って効かなくなることがない。
///     加算ブレンドでは out = src.rgb * src.a + dst.rgb のため、RGB を落とすと消える。
struct VFXMaterialEnvelope {
    bool enabled = true;

    bool        useColorGradient = true;
    std::string colorParamName   = "albedo";
    ParticleGradient colorGradient;

    /// 追加で動かしたいシェーダー変数 1 本 (任意)。空なら書かない。
    /// 例: Surface/Dissolve.hlsl の "alphaCutoff" (0 = 表示, 1 = 消滅)。
    std::string   paramName;
    ParticleCurve paramCurve;

    const char* GetTypeName() const { return "VFX Material Envelope"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("useColorGradient", useColorGradient);
        r.Field("colorParamName", colorParamName);
        r.Field("colorGradient", colorGradient);
        r.Field("paramName", paramName);
        r.Field("paramCurve", paramCurve);
    }
};

/// 時間に沿ったデカールの濃さ。DecalComponent::opacity を書く。
///
/// 進捗は DecalComponent 自身の age / lifetime から解決される (VFXElement は付けない)。
/// WHY fadeTime の線形フェードで足りないか: 焼け跡は «しばらく濃く残ってから急に消える»
///     減り方をする。線形だと置いた直後から薄まり «跡が残った» にならない。
struct VFXDecalEnvelope {
    bool enabled = true;

    /// opacity へ入る値。
    ParticleCurve fadeCurve{
        {{ {0.0f, 1.0f}, {0.65f, 0.95f}, {1.0f, 0.0f}, {1.0f, 0.0f},
           {1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f} }},
        3, ParticleCurveInterpolation::Linear };

    /// emissiveScale への倍率も動かす。組み込み経路 (materialPath が空) でのみ効く。
    bool          driveEmissive = false;
    ParticleCurve emissiveCurve;

    // --- ランタイム ---
    float baseEmissiveScale = 0.0f;
    bool  captured          = false;

    const char* GetTypeName() const { return "VFX Decal Envelope"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("fadeCurve", fadeCurve);
        r.Field("driveEmissive", driveEmissive);
        r.Field("emissiveCurve", emissiveCurve);
    }
};

} // namespace fbzz::scene
