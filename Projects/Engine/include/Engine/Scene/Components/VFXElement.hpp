/// @file    VFXElement.hpp
/// @brief   VFX の生存窓 (VFXElement) と、そこから実コンポーネントを動かすエンベロープ群。
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// @note 時間の正本は 1 オブジェクト 1 箇所という規約。自前で時間を持つ ParticleEmitter/Trail/
///       Decal には付けず、時間を持たない Light/FlowField/MeshRenderer/ScreenEffect 系に付ける。
/// @note エンベロープは VFX 専用にしない: DecalComponent の age 等からも進捗を解決できるため
///       (VFXSystem::ResolveProgress)、VFXElement が無いオブジェクトでも使える。
#pragma once
#include <Engine/Scene/ParticleCurve.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <string>

namespace fbzz::scene {

/// エンベロープが書き換える前の値を 1 組だけ覚えておく箱。
/// @note 捕獲/復元が別関数で人手管理だと書き忘れ事故が起きる (捕獲忘れ→値がループ毎に縮む、
///       復元忘れ→効果終了後も戻らない)。型に通して防ぎ、シーンへは保存しない (実行中の
///       元の値でしかなく、保存すると書き換え途中の値が焼き付く)。
template<class T>
struct VFXCaptured {
    T    value{};
    bool captured = false;

    /// 初回だけ現在値を覚える。2 回目以降は何もしない。
    void Capture(const T& current)
    {
        if (captured) return;
        value = current;
        captured = true;
    }

    /// 覚えていれば書き戻す。覚えていなければ何もしない。
    /// @return 書き戻したか
    bool Restore(T& target) const
    {
        if (!captured) return false;
        target = value;
        return true;
    }
};

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

    /// @name ランタイム
    /// @{

    /// 窓の進捗 [0,1]。窓の外では 0 (開始前) または 1 (終了後)。
    float progress = 0.0f;
    /// weightCurve.Evaluate(progress)。窓の外では 0。
    float weight = 0.0f;
    /// trigger 待ちが解けた時刻 [秒]。負なら未発火。
    float triggeredAt = -1.0f;
    /// 前フレームの窓内時刻。loop の折り返しを検出して頭出しするために持つ。
    /// @note ループする窓は active のままなので、SetActive の切り替わりに頼ると一度も頭出しされない。
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
    /// @}
};

/// 時間に沿った閃光。LightComponent の intensity / color を書き換える。
/// @note 基準値は Inspector の LightComponent の値 (=ピーク) を初回適用時に捕まえ、毎フレーム倍率を
///       掛け直す。ここに複製すると LightComponent.intensity と二重管理になり食い違う。
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

    /// @name ランタイム
    /// @{
    /// 書き換える前の LightComponent の値。
    struct Base {
        float         intensity = 0.0f;
        math::Vector3 color     = { 1.0f, 1.0f, 1.0f };
    };
    VFXCaptured<Base> base;

    const char* GetTypeName() const { return "VFX Light Envelope"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("useIntensityCurve", useIntensityCurve);
        r.Field("intensityCurve", intensityCurve);
        r.Field("useColorGradient", useColorGradient);
        r.Field("colorGradient", colorGradient);
    }
    /// @}
};

/// 時間に沿った膨張・収縮。localScale へ倍率を掛ける。
/// 衝撃波シェル・斬撃の板ポリ・魔法陣の立ち上がりがこれ 1 つで作れる。
struct VFXTransformEnvelope {
    bool enabled = true;

    /// localScale への倍率。既定は 0.1 倍から 4 倍へ、頭で一気に開いて減速する形。
    /// @note 線形だと «風船が膨らむ» 動きになり衝撃波に見えないため、既定は減速カーブ。
    bool useScaleCurve = true;
    ParticleCurve scaleCurve{
        {{ {0.0f, 0.1f}, {0.35f, 2.8f}, {1.0f, 4.0f}, {1.0f, 4.0f},
           {1.0f, 4.0f}, {1.0f, 4.0f}, {1.0f, 4.0f}, {1.0f, 4.0f} }},
        3, ParticleCurveInterpolation::Smooth };

    /// @name ランタイム
    /// @{
    /// 書き換える前の localScale。
    VFXCaptured<math::Vector3> base;

    const char* GetTypeName() const { return "VFX Transform Envelope"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("useScaleCurve", useScaleCurve);
        r.Field("scaleCurve", scaleCurve);
    }
    /// @}
};

/// Mesh シェルのマテリアル未割当時に使う既定 .mat。加算・両面の Unlit なので、置いただけで
/// 衝撃波シェルとして成立する。
/// @note 共通の Fallback.mat (不透明マゼンタ、«壊れている» 表示) はエフェクトとして使えないため分ける。
inline constexpr const char* kVFXMeshFallbackMaterial =
    "Assets/Materials/Fallback/VFXMeshFallback.mat";

/// 時間に沿ったマテリアル値の書き換え。MaterialComponent::paramOverrides へ書く。
/// @note 既定の書き先が "albedo" な理由: どの .mat にも必ずある共通パラメーターで、差し替えても
///       黙って効かなくならない。加算ブレンド (out = src.rgb*src.a + dst.rgb) では RGB を落とすと消える。
struct VFXMaterialEnvelope {
    bool enabled = true;

    bool        useColorGradient = true;
    std::string colorParamName   = "albedo";
    ParticleGradient colorGradient;

    /// 追加で動かしたいシェーダー変数 1 本 (任意)。空なら書かない。
    /// 例: Surface/Dissolve.hlsl の "alphaCutoff" (0 = 表示, 1 = 消滅)。
    std::string   paramName;
    ParticleCurve paramCurve;

    /// @name ランタイム
    /// @{
    /// 自分が paramOverrides へ書き込んだキー。復元でこれだけを取り除く。
    /// @note 他のエンベロープと違い元の値を覚えず書きっぱなしだと、プール再利用時に前回最後の
    ///       値 (フェード後の透明等) が次の Apply まで 1 フレーム見えるバグになる (VFXCaptured
    ///       を作った理由の実例)。
    /// @note paramOverrides は «上書き» なので元の状態は «キーが無い» こと。値でなくキーを覚え、
    ///       復元は消すのが正しい (空文字の書き戻しは誤り)。
    std::string writtenColorParam;
    std::string writtenParam;

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
    /// @}
};

/// 時間に沿ったデカールの濃さ。DecalComponent::opacity を書く。進捗は DecalComponent 自身の
/// age / lifetime から解決される (VFXElement は付けない)。
/// @note 焼け跡は «しばらく濃く残ってから急に消える» 減り方をする。線形フェードだと置いた直後
///       から薄まり «跡が残った» 見た目にならない。
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

    /// @name ランタイム
    /// @{
    /// 書き換える前の emissiveScale。
    VFXCaptured<float> base;

    const char* GetTypeName() const { return "VFX Decal Envelope"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("fadeCurve", fadeCurve);
        r.Field("driveEmissive", driveEmissive);
        r.Field("emissiveCurve", emissiveCurve);
    }
    /// @}
};

} // namespace fbzz::scene
