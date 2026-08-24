/// @file    Beam.hlsl
/// @brief   極性エミッターの照射ビーム。芯・グロー・流れを 1 枚の帯で描く。
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// PSO: SOLID_NOCULL + PREMULTIPLIED + DEPTH_READ
///
/// 頂点は LineRendererComponent が組む帯で、uv.x = 銃口から着弾点への進み [0,1]、
/// uv.y = 帯の横断 [0,1]。素材 (T_Beam_Stripe) は X 方向にシームレスなので、
/// uv.x をタイル + スクロールさせると «線が流れている» が出る。
///
/// WHY 事前乗算 (PREMULTIPLIED) か:
///   ビームは «背景を隠す芯» と «背景へ光を足すだけの縁» が 1 枚の中に同居する。
///   ALPHA_BLEND だと縁が背景を薄める方向に働いて濁り、ADDITIVE だと芯まで透けて
///   線の中心が読めない。out = src.rgb + dst.rgb * (1 - src.a) なら、
///   rgb を «足す光»、a を «隠す量» として独立に出せる。
///   芯 (a≈1) は背景を隠して光り、縁 (a≈0) は純粋な加算グローになる。
///
///   なお PREMULTIPLIED を選べるようになったのは、GameplayComponentSystems の
///   applyMaterial が LineRenderer の blendMode を ALPHA_BLEND で焼き付けるのを
///   やめたため。それ以前は .mat に何を書いても通らなかった。
///
/// WHY 帯電の «びりびり» を帯の外ではなく断面の中で出すか:
///   線を折って暴れさせると頂点が増え、当たり判定 (PolarityBeam の線分) と芯の位置が
///   食い違って «見た目は貫いたのに塗れない» が起きる。蛇行・途切れ・明滅を uv だけで
///   作れば、判定は直線のまま、絵だけが生きた電流になる。帯の «外» を走る放電は
///   ElectricArcBundle が別の線として重ねる — 役割が違うので層も分ける。
///
/// WHY 企画書 12.2 のために芯を白へ振らないか:
///   「発光を強くしすぎると白飛びして赤と青の区別がつかなくなる」。芯の明るさは
///   albedo (＝極性色 × coreBrightness) の比率を保ったまま上げる。白を混ぜると
///   最も明るい画素が無彩色になり、遠距離で ＋ と − が同じ線に見える。
///
/// WHY cbuffer の名前を変えてはいけないか:
///   ShaderDescriptor は PS バイトコードを GetConstantBufferByName("MaterialConstants")
///   で引いて変数表を作る。名前が違うと表が空のまま «有効な Descriptor» が返り、
///   MaterialInstance::Set* が全部の per-instance 上書きを黙って捨てる
///   (詳細は同フォルダの ElectricArc.hlsl のヘッダー)。

#include "Common/Binding.hlsli"

#define FBZZ_MATERIAL_CONSTANTS
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    /// 線の色と不透明度。LineRenderer が startColor から毎フレーム書き込む。
    /// rgb は極性色 × 明るさ (HDR)、a は «芯がどれだけ背景を隠すか»。
    float4 albedo;

    /// 芯の太さ。帯の半幅に対する割合 [0,1]。
    float  coreWidth;
    /// 縁の減衰指数。大きいほど芯が細く鋭くなる。
    float  edgeFalloff;
    /// 芯の明るさ倍率。縁との差がビームの «細さ» を決める。
    float  coreBoost;
    /// 全体の明るさ倍率 (HDR)。
    float  intensity;

    /// 素材を帯の長さ方向へ何回繰り返すか。0 で 1 枚を引き伸ばす。
    /// WHY 必要か: ビームの長さは狙う先までの距離で毎フレーム変わる。uv.x は常に
    ///      [0,1] なので、タイルしないと近くを撃つほど模様が間延びする。
    float  tiling;
    /// uv.x のスクロール量。スクリプトが毎フレーム進める。
    /// WHY 時間を cbuffer から取らないか: time を持つ PostProcConstants (b6) は
    ///      ジオメトリ描画では束縛されない。ElectricArc と同じ理由で外から渡す。
    float  scroll;
    /// 銃口側の立ち上がり [0,1]。0 で切り口がそのまま出る。
    float  muzzleFade;
    /// 着弾側の減衰 [0,1]。1 に近いほど先端が細く消える。
    float  tipFade;

    // ── 帯電 (びりびり) ──────────────────────────────────────────────────────
    /// 乱れの位相。スクリプトが毎フレーム進める。
    /// WHY 時間を cbuffer から取らないか: time を持つ PostProcConstants (b6) は
    ///      ジオメトリ描画では束縛されない。外から渡すことで、ヒットストップで
    ///      画面が止まっている間はビームの乱れも止まる。
    float  phase;
    /// 芯を帯の中で左右へ蛇行させる幅。帯の半幅に対する割合 [0,1]。
    ///
    /// WHY 折れ線ではなく UV でずらすか:
    ///   線そのものを折るには頂点を増やすしかなく、太さと当たり判定の «芯»
    ///   (PolarityBeam の線分) が食い違う。断面の中で芯の位置だけを振れば、
    ///   1 枚の帯のまま «中の電流が暴れている» が出て、判定は直線のまま残る。
    float  arcAmp;
    /// 蛇行の空間周波数 [周/帯]。大きいほど細かく波打つ。
    float  arcFreq;
    /// 芯の途切れ量 [0,1]。1 に近いほど筋が断続してフィラメントらしくなる。
    float  crackle;
    /// 帯全体の明滅の深さ [0,1]。バッテリーが尽きかけるほど上げると不安定に見える。
    float  flicker;
    /// 帯を流れる粒の数。0 で粒を出さない。
    ///
    /// WHY 素材のスクロールと別に持つか:
    ///   スクロールは «模様が流れる» だけで、どこからどこへ流れているのか読めない。
    ///   等間隔の粒を走らせると «銃口から着弾点へ電荷が送られている» と読める。
    float  beadDensity;
    /// 粒の鋭さ。大きいほど点に近づき、小さいと尾を引く。
    float  beadFalloff;
};

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Material/Effects/ArcNoise.hlsli"
#include "Platform/Backend.hlsli"

Texture2D    texAlbedo   : register(TEX_ALBEDO);
SamplerState sampDefault : register(SAMPLER_DEFAULT);

struct BeamPSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

BeamPSIn VSMain(VSInput v)
{
    BeamPSIn o;
    float4 worldPos = mul(float4(v.position, 1.0f), world);
    o.svPosition = mul(worldPos, viewProjection);
    o.uv = v.uv;
    return o;
}

float4 PSMain(BeamPSIn input) : SV_Target0
{
    const float along = saturate(input.uv.x);
    // 帯の中心を 0 とした符号付きの横断座標。芯を «ずらす» ので符号を残す。
    const float centered = (input.uv.y - 0.5f) * 2.0f;

    // ── 芯の蛇行 ─────────────────────────────────────────────────────────
    // 銃口側は絞り、着弾側ほど大きく振らせる。押さえの効く根元と暴れる先端という
    // 差が付くだけで、同じ 1 本の線が «出力されている» ように見える。
    //
    // 振れ幅は «芯が帯からはみ出さない» 範囲に閉じる。はみ出すと帯の縁で芯が
    // 四角く切り落とされ、蛇行ではなく «線が途中で切れた» に見える。
    const float headroom = saturate(1.0f - coreWidth);
    const float swing = arcAmp * headroom * (0.25f + 0.75f * along);
    const float snake = (ArcFbm(along * max(arcFreq, 0.0f) + phase * 3.1f) - 0.5f) * 2.0f * swing;
    // 芯からの距離。断面の形はすべてここから作る。
    const float across = saturate(abs(centered - snake));
    // グローは芯の半分だけ追う。完全に追うと帯ごと動いて «線がぶれている» に見え、
    // 追わないと芯がグローの外へ出て «帯から糸がはみ出した» に見える。
    const float glowAcross = saturate(abs(centered - snake * 0.5f));

    // ── 素材 ─────────────────────────────────────────────────────────────
    // 素材は無彩色 (RGB = 輝度 / A = カバレッジ)。色は albedo から乗る。
    // tiling = 0 は «1 枚を全長へ引き伸ばす»。ここで along へ倒しておかないと u が
    // scroll だけになり、素材の 1 列だけを縦に引き伸ばした «縞のない帯» になる。
    const float tiles = max(tiling, 0.0f);
    const float2 uv = float2((tiles > 0.0f ? along * tiles : along) + scroll, input.uv.y);
    const float4 stripe = texAlbedo.Sample(sampDefault, uv);

    // ── 断面: 白熱する芯 + 外へ広がるグロー ─────────────────────────────
    // 位相をずらしたノイズで芯を痩せさせる。太さが場所ごとに欠けると、同じ 1 本でも
    // «焼き切れかけたフィラメント» に見える。0 まで落ちる点が «途切れ» になる。
    const float grain     = ArcFbm(along * 26.0f + phase * 7.3f);
    const float thickness = lerp(1.0f, grain, saturate(crackle));
    const float core = 1.0f - smoothstep(0.0f, max(coreWidth * thickness, 1.0e-4f), across);
    const float glow = pow(saturate(1.0f - glowAcross), max(edgeFalloff, 0.01f));

    // ── 帯を流れる粒 ─────────────────────────────────────────────────────
    // frac の鋸波を折り返して «粒の中心で 0 になる距離» にする。等間隔なので
    // 数えられる粒に見え、銃口から着弾点へ電荷が送られているように読める。
    float bead = 0.0f;
    if (beadDensity > 0.0f) {
        const float lane     = along * beadDensity + scroll;
        const float distance = abs(frac(lane) - 0.5f) * 2.0f;
        bead = pow(saturate(1.0f - distance), max(beadFalloff, 1.0f));
    }
    // 断面にも乗せる。帯いっぱいに光らせると «帯が明滅している» になって粒に見えない。
    const float beadShape = bead * pow(saturate(1.0f - across), 1.6f);

    // ── 長手方向 ─────────────────────────────────────────────────────────
    // 銃口側は立ち上げて発射口へ潜り込ませる。絞らないと帯の切り口が四角いまま残り、
    // 銃から板が生えているように見える。
    const float muzzle = smoothstep(0.0f, max(muzzleFade, 1.0e-4f), along);
    // 着弾側は先細り。当たった «点» は着弾エフェクトが担うので、線は手前で譲る。
    // NOTE: 下限を入れているのは 0 のとき smoothstep の上下端が一致して 0 除算になるため。
    const float tip    = 1.0f - smoothstep(1.0f - max(tipFade, 1.0e-4f), 1.0f, along);
    const float ends   = muzzle * tip;

    // ── 合成 ─────────────────────────────────────────────────────────────
    // 帯全体の明滅。along を混ぜないと «一定周期で点滅するだけ» になるので、
    // 場所によってわずかにずらして «脈打っている» にする。
    const float flick = lerp(1.0f, 0.45f + 1.05f * ArcNoise(phase * 17.0f + along * 1.7f),
                             saturate(flicker));

    // rgb = «足す光»。芯とグローの比が線の細さそのものなので、両方に素材の輝度を掛ける。
    const float shape = glow + core * max(coreBoost, 0.0f);
    float3 emit = albedo.rgb * stripe.rgb * shape * max(intensity, 0.0f) * ends * flick;
    // 粒は素材の縞に依存させない。縞の暗い列に来た粒が消えると数が合わなくなる。
    emit += albedo.rgb * beadShape * 2.2f * max(intensity, 0.0f) * ends;

    // a = «隠す量»。芯だけが背景を隠し、縁は加算グローとして素通しにする。
    // WHY glow を少しだけ混ぜるか: 芯だけを隠す量にすると、芯の外側が完全な加算になり、
    //     明るい背景の上でビームが消える。わずかに隠しておくと、どんな背景でも線が残る。
    float occlusion = saturate(core + glow * 0.25f + beadShape * 0.5f)
                    * stripe.a * albedo.a * ends;
    // 太さのムラは明るさよりアルファに強く効かせる。明るさだけを落とすと、
    // 芯が細くならずに «灰色の帯» になって途切れて見えない。
    occlusion *= lerp(1.0f, thickness, saturate(crackle) * 0.75f);

    // 完全に何も足さない画素まで帯として描くと、透明なのに深度・ブレンドの帯域だけ食う。
    clip(max(occlusion, dot(emit, float3(1.0f, 1.0f, 1.0f))) - 0.002f);
    return float4(emit, occlusion);
}
