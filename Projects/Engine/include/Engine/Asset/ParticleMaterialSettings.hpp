/// @file    ParticleMaterialSettings.hpp
/// @brief   .mat の [particle] テーブル — パーティクルの «見た目» の正本
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// WHY コンポーネントから切り出すか:
///   ブレンド・フリップブック・歪み・煙・自己影は «その素材がどう見えるか» であって
///   «いつどこに粒を出すか» ではない。同じ素材を 5 個のエミッターで使うたびに 34 個の
///   値を貼り直すのは現実的でないうえ、以前は blendMode だけ .mat が毎フレーム
///   コンポーネントを上書きしており、Inspector で変えても戻る・.mat の値がシーンへ
///   焼き付く、という «正本が 2 つある» 典型的な壊れ方をしていた。
///   見た目は .mat に 1 か所だけ持たせ、ParticleEmitter は発生と運動だけを持つ。
///
/// WHY [params] ではなく専用テーブルか:
///   ここにある値の多くはシェーダーだけでなく **エンジンが分岐に使う** —
///   フリップブックの UV 矩形は CPU シミュレーションが組み立て、distortion は
///   パスがシーン色を退避するか決め、selfShadowStrength は専用 RT を回すか決める。
///   [params] は «シェーダー変数名 → float 列» なので型も既定値も持てず、
///   エンジン側が文字列で引き直すことになる。型付きで持てば Inspector にも
///   コンボやスライダーをそのまま出せる。
///   純粋にシェーダーだけが読む値 (カスタムシェーダーの独自パラメータ) は
///   従来どおり [params] を使う — そちらは b2 の MaterialConstants へ名前で束縛される。
#pragma once

#include <Engine/Scene/ScriptProxy/ScriptParticleProxy.hpp> // Particle* 列挙
#include <string>

namespace fbzz::asset {

/// .mat の [particle] テーブル。既定値は «この機能を使っていない» 状態で、
/// 旧 ParticleEmitter の既定と一致させてある (移行しても見た目が変わらないため)。
struct ParticleMaterialSettings {
    // ── 合成 ──
    // NOTE: 実際のブレンドは .mat トップレベルの blend_mode が決める。
    //       ここには持たない (同じ意味の値を 2 か所に置かない)。

    // ── アルファの取り出し方 ──
    /// テクスチャのどこを不透明度として読むか。値は Rendering/Mask.hlsli の FBZZ_MASK_*。
    scene::ParticleAlphaSource alphaSource = scene::ParticleAlphaSource::TextureAlpha;

    // ── フリップブック (テクスチャシートアニメーション) ──
    /// アトラスの分割数。1x1 なら 1 枚絵として扱う。
    int spriteColumns = 1;
    int spriteRows    = 1;
    int spriteStartFrame = 0;
    /// 0 は「最終フレームまで」を意味する (途中で止めたいときだけ指定する)。
    int spriteEndFrame   = 0;
    scene::ParticleFlipbookMode flipbookMode = scene::ParticleFlipbookMode::Lifetime;
    float flipbookFramesPerSecond = 24.0f;
    /// 隣接コマを線形補間する。低フレームレートのアトラスを滑らかに見せる。
    bool flipbookFrameBlending = false;
    /// 粒子ごとに再生位相をずらす。同時に湧いた煙が全部同じコマで回るのを防ぐ。
    bool spriteRandomStartFrame = false;
    /// アトラスの各行を「見た目の異なるバリエーション」として扱い、粒子ごとに 1 行を選ぶ。
    /// 選ばれた行の中だけでアニメーションする (spriteStartFrame/EndFrame より優先)。
    bool spriteRandomRow = false;
    /// Motion Vector アトラスは各フレームの RG を [-1,1] 速度として読み、隣接コマを双方向 warp する。
    /// アトラスは [textures] の tex5 に置く。
    bool motionVectorFlipbook = false;
    float motionVectorStrength = 1.0f;

    // ── ソフトパーティクル ──
    /// 背景との交差線を深度差でぼかす。板が地面へ突き刺さって見えるのを防ぐ。
    bool softParticles = false;
    float softParticleFadeDistance = 0.5f;

    // ── 歪み (熱陽炎) ──
    /// 背景を屈折させる。有効にするとパスがシーン色を退避し、合成はアルファへ倒れる。
    bool distortion = false;
    float distortionStrength = 0.015f;
    /// 色収差量 [画面 UV]。RGB を歪み方向へずらして屈折の分散を出す。0 で無効。
    float distortionChromatic = 0.0f;
    /// 歪み専用ノーマルマップは [textures] の normal に置く。未設定なら albedo の RG を使う。

    // ── 煙の散乱 (lit smoke) ──
    /// ビルボードの疑似法線で照明応答させる。volumetric とは役割が重複するため排他。
    bool sixWayLighting = false;
    float lightingStrength = 1.0f;
    /// 巻き込み拡散。光を透かす媒質では明暗の境界が N·L=0 で切れない。
    float smokeWrap = 0.5f;
    /// 逆光透過 (前方散乱)。煙が「向こう側の光で縁から光る」効果。
    float smokeTransmission = 0.0f;
    float smokeBackScatterPower = 4.0f;

    // ── ボリュメトリック煙 ──
    /// ビルボード内で球状密度場をレイマーチして厚みを出す。
    bool volumetric = false;
    int volumetricSteps = 8;
    float volumetricDensity = 1.0f;
    float volumetricAnisotropy = 0.3f;
    float volumetricNoiseScale = 2.0f;

    // ── 影 ──
    /// 他の物体が落とす影を受ける。
    bool receiveShadows = false;
    float shadowStrength = 1.0f;
    /// 自分自身の密度で減光する。0 で無効 (専用 RT を回さない)。
    float selfShadowStrength = 0.0f;

    // ── 明るさ ──
    /// 粒子色に掛かる倍率。HDR (1 超) にするとブルームが拾う。
    float emissiveScale = 1.0f;
};

} // namespace fbzz::asset
