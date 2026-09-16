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
#include <Math/Vector3.hpp>
#include <string>
#include <string_view>

namespace fbzz::asset {

/// フリップブック (テクスチャシートアニメーション)。.mat の [particle] にはフラットな
/// キー (sprite_columns ...) のまま保存する。
///
/// WHY 塊にするか:
///   この 11 個は «アトラスのどのコマをいつ出すか» という 1 つの仕事にしか使われない。
///   平たく並べていた頃は、再生範囲の丸め (end = 0 を «最後まで» と読む・範囲外を詰める) を
///   CPU 経路と GPU 定数バッファがそれぞれ書いており、Inspector のプレビューを作ると
///   3 本目になるところだった。型と評価関数を 1 つにして 3 者に同じ答えを出させる。
struct ParticleFlipbookSettings {
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
    /// Motion Vector アトラスは各フレームの RG を [-1,1] として読み、隣接コマを双方向 warp する。
    /// アトラスは [textures] の tex5 に置く。保存値の規約は FlipbookMotionVectorEncoding.hpp。
    bool motionVectorFlipbook = false;
    /// MV アトラスを作ったときの最大移動量 S (Atlas UV)。生成器が返す recommendedStrength を入れる。
    /// 典型値は 0.002〜0.02。既定の 0 は «warp しない» (普通のコマ補間と同じ)。
    /// WHY 0 か: 旧既定の 1 は «Atlas 全幅ぶりずらす» 意味になり、MV を有効にした瞬間に絵が破綻する。
    float motionVectorStrength = 0.0f;

    /// 分割数 0 以下を 1 として数えたコマ数。
    [[nodiscard]] int FrameCount() const
    {
        return (spriteColumns > 1 ? spriteColumns : 1) * (spriteRows > 1 ? spriteRows : 1);
    }
};

/// 丸め済みの再生範囲。first / last はアトラス全体の通し番号 (左上から行優先)。
struct FlipbookFrameRange {
    int columns = 1;
    int rows    = 1;
    int first   = 0;
    int last    = 0;
};

/// ある粒子がいま出すコマ。blend は frame → nextFrame への補間率 (Frame Blending 無効なら 0)。
struct FlipbookFrameSample {
    int   frame     = 0;
    int   nextFrame = 0;
    float blend     = 0.0f;
};

/// 分割数 0 や範囲外の Start / End を丸め、spriteEndFrame = 0 を «最後まで» と読む。
/// @note Random Row の «行で範囲を上書きする» 規則は粒子ごとなのでここでは適用しない。
[[nodiscard]] FlipbookFrameRange ResolveFlipbookRange(const ParticleFlipbookSettings& flipbook);

/// 1 粒子ぶんのコマを決める。CPU シミュレーションと Inspector プレビューの正本。
/// @param normalizedAge 寿命に対する経過 [0,1] (Lifetime が使う)
/// @param ageSeconds    生まれてからの秒数 (FPS / Ping Pong が使う)
/// @param spriteSeed    粒子固有の乱数 [0,1] (Random Row / Random Start / Random が使う)
/// @note GPU 経路 (ParticleGpuSim.cs.hlsl) は同じ規則を HLSL で持つ。変えるなら両方直すこと。
[[nodiscard]] FlipbookFrameSample EvaluateFlipbookFrame(const ParticleFlipbookSettings& flipbook,
                                                        float normalizedAge,
                                                        float ageSeconds,
                                                        float spriteSeed);

/// .mat の [particle] テーブル。既定値は «この機能を使っていない» 状態で、
/// 旧 ParticleEmitter の既定と一致させてある (移行しても見た目が変わらないため)。
struct ParticleMaterialSettings {
    // ── 合成 ──
    // NOTE: 実際のブレンドは .mat トップレベルの blend_mode が決める。
    //       ここには持たない (同じ意味の値を 2 か所に置かない)。

    // ── アルファの取り出し方 ──
    /// テクスチャのどこを不透明度として読むか。値は Rendering/Mask.hlsli の FBZZ_MASK_*。
    scene::ParticleAlphaSource alphaSource = scene::ParticleAlphaSource::TextureAlpha;

    ParticleFlipbookSettings flipbook;

    // ── ソフトパーティクル ──
    /// 背景との交差線を深度差でぼかす。板が地面へ突き刺さって見えるのを防ぐ。
    bool softParticles = false;
    float softParticleFadeDistance = 0.5f;

    // ── カメラ距離フェード ──
    /// カメラから cameraFadeNear より近い粒子を薄くする [ワールド単位]。
    /// near で 0・far で 1 まで戻る。カメラが煙へ突っ込んだときに 1 枚の板で
    /// 画面全体が埋まるのを防ぐ。
    ///
    /// near >= far (既定の 0 / 0 を含む) は «この素材は距離フェードを使わない» の意味。
    /// WHY 有効フラグ (bool) を持たないか:
    ///   ソフトパーティクルと違い、この 2 値はシェーダー内で分岐なしに «使わない» を
    ///   表現できる (near >= far でフェードが常に 1)。フラグを足すと «有効なのに
    ///   near == far» という無意味な組み合わせが Inspector に現れる。
    float cameraFadeNear = 0.0f;
    float cameraFadeFar  = 0.0f;

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
    /// 6 方向ライトマップ (Volume Flipbook Baker の _6wayP / _6wayN) で陰影を付ける。
    /// albedo が Positive (右, 上, 奥, α)、emissive スロットが Negative (左, 下, 手前, 発光マスク)。
    /// sixWayLighting (疑似法線) より優先する。規約は SixWayLighting.hpp。
    bool sixWayMaps = false;
    /// Negative の A (発光マスク) に掛ける色 (リニア HDR)。炎の芯を影の中でも光らせる。
    math::Vector3 sixWayEmissionColor = { 0.0f, 0.0f, 0.0f };

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

    // ── 点光源 ──
    /// Point / Spot / 面光源 (クラスタ) を粒子の中心で受ける。煙が近くの炎や松明に照らされる。
    /// 1 画素ごとにライト一覧を走査するので、画面を覆う煙では重くなる。
    bool punctualLighting = false;

    // ── 明るさ ──
    /// 粒子色に掛かる倍率。HDR (1 超) にするとブルームが拾う。
    float emissiveScale = 1.0f;
};

// ── materialPath はテクスチャを受けない、という規約を守るための道具 ──
//
// ParticleEmitter::materialPath は .mat 専用で、テクスチャは .mat の [textures] から来る。
// ところが «貼りたいのは .png» という要求は消えないので、Editor の素材欄は
// テクスチャを落とすと決まった名前の .mat へ包んでからパスを書く。
//
// WHY 命名規則をエンジン側へ置くか:
//   包む側 (Editor) と、«テクスチャが materialPath に入ってしまった» ときに
//   救う側 (ParticlePass) が別々に名前を組み立てると、移行先が食い違って
//   «作った .mat があるのに見つけられない» が起きる。規則は 1 か所に置く。

/// パスがテクスチャか (.png / .tga / .dds / .jpg / .jpeg)。
[[nodiscard]] bool IsParticleTexturePath(std::string_view path);

/// テクスチャ 1 枚に対応する Particle 用 .mat の **決まった名前**。
///
/// "Assets/.../flame_03.png" + Additive → "Assets/Materials/Particles/Flame03_Additive.mat"
/// 決定的なので、同じテクスチャ・同じブレンドなら常に同じ .mat を指す。
/// @note 実在するかは確かめない。作る側と探す側で «どこを見るか» を揃えるためだけの関数。
[[nodiscard]] std::string ParticleMaterialPathForTexture(std::string_view texturePath,
                                                         scene::ParticleBlendMode blend);

} // namespace fbzz::asset
