/// @file    VFXElement.hpp
/// @brief   VFX の生存窓 (VFXElement) と、そこから実コンポーネントを動かすエンベロープ群。
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// @note 時間の正本は 1 オブジェクト 1 箇所という規約。自前で時間を持つ ParticleEmitter/Trail/
/// @note Decal には付けず、時間を持たない Light/FlowField/MeshRenderer/ScreenEffect 系に付ける。
/// @note エンベロープは VFX 専用にしない: DecalComponent の age 等からも進捗を解決できるため
/// @note (VFXSystem::ResolveProgress)、VFXElement が無いオブジェクトでも使える。
#pragma once
#include <Engine/Scene/VFX/ParticleCurve.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <string>

namespace fbzz::scene {

/// @note エンベロープが書き換える前の値を 1 組だけ覚えておく箱。
/// @note 捕獲/復元が別関数で人手管理だと書き忘れ事故が起きる (捕獲忘れ→値がループ毎に縮む、
/// @note 復元忘れ→効果終了後も戻らない)。型に通して防ぎ、シーンへは保存しない (実行中の
/// @note 元の値でしかなく、保存すると書き換え途中の値が焼き付く)。
template<class T>
struct VFXCaptured {
    T    value{}; ///< エンベロープ適用前に保持する元の値。
    bool captured = false; ///< value に復元可能な値を取得済み。

    /// @note 初回だけ現在値を覚える。2 回目以降は何もしない。
    void Capture(const T& current)
    {
        if (captured) {
            return;
        }
        value = current;
        captured = true;
    }

    /// @note 覚えていれば書き戻す。覚えていなければ何もしない。
    /// @return 書き戻したか
    bool Restore(T& target) const
    {
        if (!captured) {
            return false;
        }
        target = value;
        return true;
    }
};

/// @note VFX ルートの時刻に対する、この GameObject の生存窓。
///
/// @note 窓の中だけ GameObject が active になり、weight に weightCurve の値が入る。
/// @note 旧 .vfx の startOffset / duration と、ScreenEffect の fadeIn/fadeOut ・
/// @note CameraShake の falloffPower ・TimeScale の blendIn/blendOut を 1 本のカーブへ畳んだもの。
struct VFXElement {
    bool enabled = true; ///< このエフェクトの生存窓をルート VFX の時間で管理する。

    float startDelay = 0.0f; ///< VFX 先頭からの遅延 [秒]。
    float duration = 1.0f; ///< 窓の長さ [秒]。0 以下ならルートが終わるまで開いたままにする。
    bool loop = false; ///< 窓の終わりで頭出しして繰り返す。

    /// @note 窓の進捗 0..1 に対する重み。既定は矩形 (常に 1)。
    /// @note VFXScreenEffect / VFXCameraShake / VFXTimeScale の weight へそのまま入り、
    /// @note エンベロープ各種はこれではなく進捗そのものを見る。
    ParticleCurve weightCurve{
        {{ {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f},
           {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f} }},
        2, ParticleCurveInterpolation::Linear }; ///< 生存窓の進捗を、画面・揺れ・時間演出へ渡す適用重みに変える。

    /// @note 空でなければ時間ではなく VFXComponent::Trigger(name) で開始する。
    /// @note 旧 .vfx の OnCollision / OnDeath / OnAnimationEvent / OnTrigger の置き換え。
    std::string trigger; ///< 時間指定の代わりに開始の合図に使うイベント名。空なら時間で開始する。

    /// @name ランタイム
    /// @{

    float progress = 0.0f; ///< 窓の進捗 [0,1]。窓の外では 0 (開始前) または 1 (終了後)。
    float weight = 0.0f; ///< weightCurve.Evaluate(progress)。窓の外では 0。
    float triggeredAt = -1.0f; ///< trigger 待ちが解けた時刻 [秒]。負なら未発火。
    /// @note 前フレームの窓内時刻。loop の折り返しを検出して頭出しするために持つ。
    /// @note ループする窓は active のままなので、SetActive の切り替わりに頼ると一度も頭出しされない。
    float lastLocalTime = -1.0f; ///< ループの折り返し検出に使う前フレームの生存窓内時刻（秒）。

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

/// @note 時間に沿った閃光。LightComponent の intensity / color を書き換える。
/// @note 基準値は Inspector の LightComponent の値 (=ピーク) を初回適用時に捕まえ、毎フレーム倍率を
/// @note 掛け直す。ここに複製すると LightComponent.intensity と二重管理になり食い違う。
struct VFXLightEnvelope {
    bool enabled = true; ///< 光源の強度や色へ進捗に応じたエンベロープを適用する。

    bool useIntensityCurve = true; ///< intensity への倍率。0..1 想定だが 1 超で増幅もできる。
    ParticleCurve intensityCurve{
        {{ {0.0f, 1.0f}, {0.12f, 0.55f}, {1.0f, 0.0f}, {1.0f, 0.0f},
           {1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f} }},
        3, ParticleCurveInterpolation::Linear }; ///< 生存窓の進捗から保存した光源強度に掛ける倍率を求める。

    /// @note 色の推移。alpha は明るさ倍率として rgb へ乗算する
    /// @note (白熱 → 橙 → 暗赤 のような色温度変化を 1 本で作れるようにするため)。
    bool useColorGradient = false;
    ParticleGradient colorGradient; ///< 効果進捗から求める光源の色。アルファは強度の倍率にする。

    /// @name ランタイム
    /// @{
    /// @note 書き換える前の LightComponent の値。
    struct Base {
        float         intensity = 0.0f; ///< 光源の強度を書き換える前に保存する基準値。
        math::Vector3 color     = { 1.0f, 1.0f, 1.0f }; ///< 光源の色を書き換える前に保存する基準 RGB 値。
    };
    VFXCaptured<Base> base; ///< 光源の元の強度と色を取得して復元するための状態。

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

/// @note 時間に沿った膨張・収縮。localScale へ倍率を掛ける。
/// @note 衝撃波シェル・斬撃の板ポリ・魔法陣の立ち上がりがこれ 1 つで作れる。
struct VFXTransformEnvelope {
    bool enabled = true; ///< ローカルスケールへ進捗に応じたエンベロープを適用する。

    /// @note localScale への倍率。既定は 0.1 倍から 4 倍へ、頭で一気に開いて減速する形。
    /// @note 線形だと «風船が膨らむ» 動きになり衝撃波に見えないため、既定は減速カーブ。
    bool useScaleCurve = true;
    ParticleCurve scaleCurve{
        {{ {0.0f, 0.1f}, {0.35f, 2.8f}, {1.0f, 4.0f}, {1.0f, 4.0f},
           {1.0f, 4.0f}, {1.0f, 4.0f}, {1.0f, 4.0f}, {1.0f, 4.0f} }},
        3, ParticleCurveInterpolation::Smooth }; ///< 生存窓の進捗からローカルスケールに掛ける倍率を求める。

    /// @name ランタイム
    /// @{
    /// @note 書き換える前の localScale。
    VFXCaptured<math::Vector3> base; ///< ローカルスケールの元の値を取得して復元するための状態。

    const char* GetTypeName() const { return "VFX Transform Envelope"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("useScaleCurve", useScaleCurve);
        r.Field("scaleCurve", scaleCurve);
    }
    /// @}
};

/// @note Mesh シェルのマテリアル未割当時に使う既定 .mat。加算・両面の Unlit なので、置いただけで
/// @note 衝撃波シェルとして成立する。
/// @note 共通の Fallback.mat (不透明マゼンタ、«壊れている» 表示) はエフェクトとして使えないため分ける。
inline constexpr const char* kVFXMeshFallbackMaterial =
    "Assets/Materials/Fallback/VFXMeshFallback.mat";

/// @note 時間に沿ったマテリアル値の書き換え。MaterialComponent::paramOverrides へ書く。
/// @note 既定の書き先が "albedo" な理由: どの .mat にも必ずある共通パラメーターで、差し替えても
/// @note 黙って効かなくならない。加算ブレンド (out = src.rgb*src.a + dst.rgb) では RGB を落とすと消える。
struct VFXMaterialEnvelope {
    bool enabled = true; ///< 材質の上書きパラメータへ時間変化を適用する。

    bool        useColorGradient = true; ///< 効果進捗から求めた色を colorParamName へ書き込む。
    std::string colorParamName   = "albedo"; ///< 色グラデーションの書き込み先となるシェーダーのパラメータ名。
    ParticleGradient colorGradient; ///< 効果進捗に応じて材質へ書き込む RGBA 色。

    /// @note 追加で動かしたいシェーダー変数 1 本 (任意)。空なら書かない。
    /// @note 例: Surface/Dissolve.hlsl の "alphaCutoff" (0 = 表示, 1 = 消滅)。
    std::string   paramName; ///< スカラーの時間変化を書き込むパラメータ名。空なら書き込まない。
    ParticleCurve paramCurve; ///< 効果進捗から paramName へ書くスカラー値を求めるカーブ。

    /// @name ランタイム
    /// @{
    /// @note 自分が paramOverrides へ書き込んだキー。復元でこれだけを取り除く。
    /// @note 他のエンベロープと違い元の値を覚えず書きっぱなしだと、プール再利用時に前回最後の
    /// @note 値 (フェード後の透明等) が次の Apply まで 1 フレーム見えるバグになる (VFXCaptured
    /// @note を作った理由の実例)。
    /// @note paramOverrides は «上書き» なので元の状態は «キーが無い» こと。値でなくキーを覚え、
    /// @note 復元は消すのが正しい (空文字の書き戻しは誤り)。
    std::string writtenColorParam; ///< 復元時に解放する、この処理が上書きした色パラメータのキー。
    std::string writtenParam; ///< 復元時に解放する、この処理が上書きしたスカラーパラメータのキー。

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

/// @note 時間に沿ったデカールの濃さ。DecalComponent::opacity を書く。進捗は DecalComponent 自身の
/// @note age / lifetime から解決される (VFXElement は付けない)。
/// @note 焼け跡は «しばらく濃く残ってから急に消える» 減り方をする。線形フェードだと置いた直後
/// @note から薄まり «跡が残った» 見た目にならない。
struct VFXDecalEnvelope {
    bool enabled = true; ///< デカールの寿命に合わせて不透明度や発光を更新する。

    /// @note opacity へ入る値。
    ParticleCurve fadeCurve{
        {{ {0.0f, 1.0f}, {0.65f, 0.95f}, {1.0f, 0.0f}, {1.0f, 0.0f},
           {1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f} }},
        3, ParticleCurveInterpolation::Linear }; ///< デカール寿命の進捗から書き込む不透明度を求める。

    bool          driveEmissive = false; ///< emissiveScale への倍率も動かす。組み込み経路 (materialPath が空) でのみ効く。
    ParticleCurve emissiveCurve; ///< デカール寿命の進捗から発光の倍率を求めるカーブ。

    /// @name ランタイム
    /// @{
    /// @note 書き換える前の emissiveScale。
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

}
