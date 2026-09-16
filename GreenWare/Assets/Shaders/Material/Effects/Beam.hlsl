/// @file    Beam.hlsl
/// @brief   極性エミッターの照射ビーム。1 枚の帯を «撚られた電流の円柱» として描く。
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// PSO: SOLID_NOCULL + PREMULTIPLIED + DEPTH_READ
///
/// 頂点は LineRendererComponent が組む帯で、uv.x = 銃口から着弾点への進み [0,1]、
/// uv.y = 帯の横断 [0,1]。素材 (T_Beam_Stripe) は X 方向にシームレスなので、
/// uv.x をタイル + スクロールさせると «線が流れている» が出る。
///
/// WHY 平らな帯を «円柱» として解くか:
///   ビルボードの帯へ断面のグラデーションを塗っただけの線は、どれだけ明るくしても
///   «光っている板» にしかならない。円柱として視線が抜ける弦の長さ (chord) を厚みへ
///   掛けると、中心が濃く縁が薄いという «体積» の分布が出て、同じ 1 枚が管に見える。
///   さらに管の «壁» をガウス環で 1 本足すと、中身の詰まった棒ではなく
///   «エネルギーが通っている筒» になる。volume / shell* がその 2 つ。
///
/// WHY 芯を «撚る» か:
///   1 本の芯は太さを揺らしても «明滅する線» の域を出ない。位相をずらした複数の芯を
///   軸のまわりで回すと、手前の筋が明るく奥の筋が沈む往復が生まれ、線そのものが
///   回転して見える。断面 (uv.y) の中で位置と明暗を作るだけなので、当たり判定
///   (ビームの線分) は直線のまま動かない。
///
/// WHY «帯そのものがうねる» をここでやらないか:
///   帯の経路を曲げるのは BeamTrailRendererComponent の仕事で、そちらは折れ線の頂点を
///   動かす (両端は銃口と着弾点に固定するので、当たり判定の線分からは離れない)。
///   ここが受け持つのは «その帯の断面の中で電流がどう動くか»。arcAmp も crackle も
///   flicker も uv だけで閉じている。
///   分けてあるのは、片方だけでは «動かない管» か «中身のない管» にしかならないため。
///   帯の «外» を走る放電はさらに別で、ElectricArcBundle が線として重ねる。
///
/// WHY 事前乗算 (PREMULTIPLIED) か:
///   ビームは «背景を隠す芯» と «背景へ光を足すだけの縁» が 1 枚の中に同居する。
///   ALPHA_BLEND だと縁が背景を薄める方向に働いて濁り、ADDITIVE だと芯まで透けて
///   線の中心が読めない。out = src.rgb + dst.rgb * (1 - src.a) なら、
///   rgb を «足す光»、a を «隠す量» として独立に出せる。
///   芯 (a≈1) は背景を隠して光り、縁 (a≈0) は純粋な加算グローになる。
///
/// WHY 企画書 12.2 のために芯を白へ振り «切らない» か:
///   「発光を強くしすぎると白飛びして赤と青の区別がつかなくなる」。芯の白熱は
///   coreWhite が受け持つが、寄せるのは «最も明るい成分へ揃える» までで、明るさ自体は
///   増やさない。白を足す実装にすると最も明るい画素が無彩色になり、遠距離で ＋ と −
///   が同じ線に見える。白熱させたいときは幅を絞る (面積で稼がない) のが正解。
///
/// WHY 素材を 2 回サンプルするか (detail):
///   1 枚の縞をタイルすると、長い線ほど同じ模様の反復が読めてしまう。倍率も向きも
///   違う 2 枚目を重ねると周期が最小公倍数まで伸び、反復が目に留まらなくなる。
///
/// WHY 時間を cbuffer から取らないか:
///   time を持つ PostProcConstants (b6) はジオメトリ描画では束縛されない。phase と
///   scroll はスクリプトが毎フレーム書き込む。結果としてヒットストップで画面が
///   止まっている間はビームの乱れも止まる (時間の止まり方が絵と一致する)。
///   どちらも整数周期で巻き取られている前提で使うこと — 巻き戻りで絵が飛ばないよう、
///   位相へ掛ける係数 (braidSpeed / ringSpeed) は 1/2^n を選ぶ。
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

    // ── 断面 ────────────────────────────────────────────────────────────────
    /// 芯の太さ。帯の半幅に対する割合 [0,1]。
    float  coreWidth;
    /// 縁の減衰指数。大きいほど芯が細く鋭くなる。
    float  edgeFalloff;
    /// 芯の明るさ倍率。縁との差がビームの «細さ» を決める。
    float  coreBoost;
    /// 全体の明るさ倍率 (HDR)。
    float  intensity;

    // ── 流れ ────────────────────────────────────────────────────────────────
    /// 素材を帯の長さ方向へ何回繰り返すか。0 で 1 枚を引き伸ばす。
    /// WHY 必要か: ビームの長さは狙う先までの距離で毎フレーム変わる。uv.x は常に
    ///      [0,1] なので、タイルしないと近くを撃つほど模様が間延びする。
    ///      撚りとリングの密度もこの値を基準にするため、«1m あたりの細かさ» が
    ///      距離で変わらない (詳細は braidTwist / ringDensity)。
    float  tiling;
    /// uv.x のスクロール量。スクリプトが毎フレーム進める。
    float  scroll;
    /// 銃口側の立ち上がり [0,1]。0 で切り口がそのまま出る。
    float  muzzleFade;
    /// 着弾側の減衰 [0,1]。1 に近いほど先端が細く消える。
    float  tipFade;

    // ── 乱れ ────────────────────────────────────────────────────────────────
    /// 乱れの位相。スクリプトが毎フレーム進める。
    float  phase;
    /// 芯を帯の中で左右へ蛇行させる幅。帯の半幅に対する割合 [0,1]。
    ///
    /// WHY 帯を曲げる側 (BeamTrailRendererComponent の Wobble) と別に要るか:
    ///   向こうは管そのものの経路で、こちらは «管の中を通る芯がどこを走るか»。
    ///   帯だけを曲げると中身の無い光る紐に、断面だけを振ると硬い板の上で光が
    ///   泳ぐ絵になる。2 つが重なって初めて «電流の通っている管» として読める。
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
    /// 粒の頭の鋭さ。大きいほど点に近づき、尾はその分だけ長く伸びる。
    float  beadFalloff;

    /// この帯が芯層か裾層か。1 = 芯 / 0 = 裾。
    ///
    /// WHY 1 個のスイッチに束ねるか:
    ///   芯と裾は同じ .mat を共有するため、層ごとの差はスクリプトが per-instance で
    ///   書くしかない。撚り・管の壁・リング・白熱を «それぞれ 0 にする» 形にすると、
    ///   新しい要素を足すたびにスクリプト側へ «裾では 0» を書き足す必要があり、
    ///   1 か所書き忘れた瞬間に裾が芯の模様を持つ。層の役割を 1 個の値で渡せば、
    ///   «どの要素が芯だけのものか» の判断はこのファイルの中で閉じる。
    float  layer;

    // ── 手応え ──────────────────────────────────────────────────────────────
    /// 命中と点火の «張り» [0,1]。スクリプトが立ち上げて減衰させる。
    /// リングと閃光を増やす方向にだけ効かせる (色は動かさない)。
    float  surge;

    // ── 立体 ────────────────────────────────────────────────────────────────
    /// 円柱として厚みを積む量 [0,1]。0 で従来どおりの平らな帯。
    float  volume;
    /// 2 枚目の縞の効き [0,1]。0 で 1 枚だけの反復。
    float  detail;
    /// 芯を白熱へ寄せる量 [0,1]。0 で純粋な極性色。
    /// 1 でも «白を足す» のではなく最も明るい成分へ揃えるだけなので飽和はしない。
    float  coreWhite;

    /// 管の «壁» の位置。帯の半幅に対する割合 [0,1]。
    float  shellRadius;
    /// 壁の厚み。小さいほど硬い輪郭になる。
    float  shellWidth;
    /// 壁の明るさ。0 で壁を出さない (中身の詰まった棒になる)。
    float  shellBoost;
    /// 壁の色収差 [半幅比]。R と B を逆向きにずらして縁へ色を割る。
    ///
    /// WHY 色を割るか: 単色の光の筒は «塗った線» に見える。縁だけ波長がずれていると
    ///      «レンズを通した強い光» の記憶に当たり、同じ明るさでも密度が上がる。
    float  dispersion;

    // ── 撚り ────────────────────────────────────────────────────────────────
    /// 芯の中で撚り合わせる筋の本数 [0,4]。0 で撚らない。
    float  filaments;
    /// 撚りの半径。帯の半幅に対する割合 [0,1]。
    float  braid;
    /// 撚りの巻き数 [周/タイル]。tiling を基準にするので、近距離でも遠距離でも
    /// «1m あたり何回巻くか» が変わらない。
    float  braidTwist;
    /// 撚りが回る速さ [周/位相]。位相の巻き戻りで飛ばないよう 1/2^n を選ぶ。
    float  braidSpeed;

    // ── 衝撃波リング ────────────────────────────────────────────────────────
    /// 帯を走る輪の数 [本/タイル]。0 で出さない。
    ///
    /// WHY 粒と別に持つか: 粒は «運ばれている物» で、リングは «押し出された圧»。
    ///      前者は線に向きを与え、後者は銃口が脈動していることを伝える。
    ///      同じものにすると、どちらの読みも中途半端になる。
    float  ringDensity;
    /// 輪が進む速さ [周/位相]。負で銃口へ向かって走る。1/2^n を選ぶこと。
    float  ringSpeed;
    /// 輪の鋭さ。大きいほど細い輪になる。
    float  ringSharpness;
    /// 輪の明るさ。0 で出さない。
    float  ringBoost;

    // ── 端 ──────────────────────────────────────────────────────────────────
    /// 銃口側の閃光が届く長さ [帯比]。発射口から «噴き出している» を作る。
    float  muzzleFlare;
    /// 着弾側の閃光が届く長さ [帯比]。
    float  tipFlare;
    /// 線を «覗き込んだ» ときの増光。軸と視線が揃うほど強くなる。
    ///
    /// WHY 要るか: ビルボードの帯は真正面から見ると画面上で潰れ、いちばん近くを
    ///      撃っているのに線がいちばん弱くなる。円柱なら軸方向は光路が最も長いので、
    ///      潰れるどころか焼き付くのが正しい。
    float  axialGlint;
    float  _beamPad;
};

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Material/Effects/ArcNoise.hlsli"
#include "Platform/Backend.hlsli"

Texture2D    texAlbedo   : register(TEX_ALBEDO);
SamplerState sampDefault : register(SAMPLER_DEFAULT);

static const float kBeamTau = 6.28318530718f;

struct BeamPSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
    /// 帯の進行方向 (ワールド)。LineRenderer が区間の向きを tangent へ入れている。
    float3 axis       : TEXCOORD1;
    /// 画素からカメラへ向かうベクトル (ワールド、非正規化)。
    float3 toEye      : TEXCOORD2;
};

/// 管の «壁» を 1 本のガウス環で作る。色収差は半径をずらして 3 回呼ぶ。
float BeamShell(float radius)
{
    const float offsetFromWall = (radius - shellRadius) / max(shellWidth, 1.0e-3f);
    return exp(-offsetFromWall * offsetFromWall);
}

BeamPSIn VSMain(VSInput v)
{
    BeamPSIn o;
    float4 worldPos = mul(float4(v.position, 1.0f), world);
    o.svPosition = mul(worldPos, viewProjection);
    o.uv    = v.uv;
    o.axis  = mul(v.tangent, (float3x3)world);
    o.toEye = cameraPos - worldPos.xyz;
    return o;
}

float4 PSMain(BeamPSIn input) : SV_Target0
{
    const float along = saturate(input.uv.x);
    // 帯の中心を 0 とした符号付きの横断座標。芯を «ずらす» ので符号を残す。
    const float centered = (input.uv.y - 0.5f) * 2.0f;
    const float radius   = saturate(abs(centered));
    // 芯だけが持つ要素の重み。裾層 (layer = 0) では丸ごと消える。
    const float detailLayer = saturate(layer);

    // ── 立体: 円柱を視線が抜ける弦の長さ ────────────────────────────────────
    // 中心ほど長く、縁でゼロになる。これを厚みとして掛けるだけで、平らな帯が
    // «詰まっているもの» に見える。volume = 0 なら従来どおりの平らな配分。
    const float chord = sqrt(saturate(1.0f - radius * radius));
    const float thickness3D = lerp(1.0f, chord, saturate(volume));

    // ── 蛇行 ────────────────────────────────────────────────────────────────
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

    // ── 縞: 倍率と向きの違う 2 枚 ───────────────────────────────────────────
    // 素材は無彩色 (RGB = 輝度 / A = カバレッジ)。色は albedo から乗る。
    // tiling = 0 は «1 枚を全長へ引き伸ばす»。ここで along へ倒しておかないと u が
    // scroll だけになり、素材の 1 列だけを縦に引き伸ばした «縞のない帯» になる。
    const float tiles    = max(tiling, 0.0f);
    const float baseLane = (tiles > 0.0f ? along * tiles : along);
    const float4 stripe  = texAlbedo.Sample(sampDefault, float2(baseLane + scroll, input.uv.y));
    // 2 枚目は «粗く・逆向き»。同じ向きに流すと 2 枚が同じ縞に見えてしまう。
    //
    // WHY 送りの係数を 1/4 刻みにするか: scroll は 1024 で巻き取られる。係数を掛けた
    //     結果が整数倍でないと、巻き戻ったフレームだけ 2 枚目の模様が飛ぶ。
    const float4 counter = texAlbedo.Sample(sampDefault,
                                            float2(baseLane * 0.25f - scroll * 1.25f,
                                                   input.uv.y * 0.5f + 0.25f));
    const float3 weave = stripe.rgb * lerp(1.0f, 0.55f + 0.9f * counter.r, saturate(detail));

    // ── 芯 ──────────────────────────────────────────────────────────────────
    // 位相をずらしたノイズで芯を痩せさせる。太さが場所ごとに欠けると、同じ 1 本でも
    // «焼き切れかけたフィラメント» に見える。0 まで落ちる点が «途切れ» になる。
    const float grain     = ArcFbm(along * 26.0f + phase * 7.3f);
    const float thickness = lerp(1.0f, grain, saturate(crackle));
    const float core = 1.0f - smoothstep(0.0f, max(coreWidth * thickness, 1.0e-4f), across);

    // ── 撚り: 軸のまわりを回る筋 ────────────────────────────────────────────
    // 手前へ来た筋は明るく、奥へ回った筋は管の中身に沈む。断面の中で位置と明暗を
    // 作るだけなので、線は 1 本のまま «回転している» と読める。
    const float strandCount = clamp(filaments, 0.0f, 4.0f);
    const float twistLanes  = (tiles > 0.0f ? tiles : 1.0f) * braidTwist;
    const float strandWidth = max(coreWidth * 0.42f * thickness, 1.0e-4f);
    float braidLight = 0.0f;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        // WHY break ではなく重みで消すか: 本数は per-instance で毎フレーム変わりうる。
        //     分岐で抜くと «本数だけが違う 4 通り» が動的分岐として残り、隣接画素で
        //     経路が割れる。掛け算なら常に同じ命令列で済む。
        const float active = step((float)i + 0.5f, strandCount);
        const float spin = along * twistLanes * kBeamTau
                         + phase * braidSpeed * kBeamTau
                         + (float)i * kBeamTau / max(strandCount, 1.0f);
        // 撚りの «横位置» は sin、«奥行き» は cos。両方を同じ角度から採ることで、
        // 縁へ来た筋がちょうど暗く沈む (円周上を回っているように見える)。
        const float lateral  = sin(spin) * braid * headroom;
        const float facing   = 0.55f + 0.45f * cos(spin);
        const float distance = abs(centered - snake * 0.7f - lateral);
        const float strand   = 1.0f - smoothstep(0.0f, strandWidth, distance);
        // 筋ごとに途切れをずらす。揃えると «太さが脈動する 1 本» に戻ってしまう。
        const float breakup = lerp(1.0f, ArcFbm(along * 31.0f + phase * 9.0f + (float)i * 13.7f),
                                   saturate(crackle));
        braidLight += strand * facing * breakup * active;
    }
    // 筋が交差した点は明るくてよいが、本数だけ足し込むと «撚った回数» がそのまま
    // 白飛びの回数になる。交差が読める程度で頭を打たせる。
    braidLight = min(braidLight, 1.6f) * detailLayer;

    // ── 管の壁 ──────────────────────────────────────────────────────────────
    // R と B を逆向きにずらして縁へ色を割る。中央 (G) は必ず本来の半径で採るので、
    // 色収差を上げても壁の «位置» そのものは動かない。
    const float3 shell = float3(BeamShell(radius + dispersion),
                                BeamShell(radius),
                                BeamShell(radius - dispersion))
                       * shellBoost * detailLayer;

    // ── 裾 ──────────────────────────────────────────────────────────────────
    const float glow = pow(saturate(1.0f - glowAcross), max(edgeFalloff, 0.01f)) * thickness3D;

    // ── 衝撃波リング ────────────────────────────────────────────────────────
    // 銃口から着弾点へ «圧» が抜けていく。粒より太く短命な光として重ねる。
    float shock = 0.0f;
    if (ringDensity > 0.0f && ringBoost > 0.0f) {
        const float ringLane = along * (tiles > 0.0f ? tiles : 1.0f) * ringDensity
                             - phase * ringSpeed;
        const float sawtooth = abs(frac(ringLane) - 0.5f) * 2.0f;
        shock = pow(saturate(1.0f - sawtooth), max(ringSharpness, 1.0f));
    }
    // 輪は «管の外周が膨らんだ» ものなので、中心より壁の側で強く出す。
    const float shockShape = shock * (0.35f + 0.65f * saturate(radius / max(shellRadius, 1.0e-3f)))
                           * thickness3D * detailLayer;

    // ── 粒 ──────────────────────────────────────────────────────────────────
    // 頭を鋭く、尾を銃口側へ引く。左右対称の点だと «光っている印» にしかならず、
    // どちらへ流れているのかが粒そのものから読めない。
    float bead = 0.0f;
    if (beadDensity > 0.0f) {
        const float beadLane = along * beadDensity + scroll;
        const float behind   = saturate(1.0f - frac(beadLane));
        bead = pow(behind, max(beadFalloff, 1.0f))
             + pow(behind, max(beadFalloff, 1.0f) * 0.18f) * 0.35f;
    }
    // 断面にも乗せる。帯いっぱいに光らせると «帯が明滅している» になって粒に見えない。
    const float beadShape = bead * pow(saturate(1.0f - across), 1.6f);

    // ── 端 ──────────────────────────────────────────────────────────────────
    // 銃口側は立ち上げて発射口へ潜り込ませる。絞らないと帯の切り口が四角いまま残り、
    // 銃から板が生えているように見える。
    const float muzzle = smoothstep(0.0f, max(muzzleFade, 1.0e-4f), along);
    // 着弾側は先細り。当たった «点» は着弾エフェクトが担うので、線は手前で譲る。
    // NOTE: 下限を入れているのは 0 のとき smoothstep の上下端が一致して 0 除算になるため。
    const float tip    = 1.0f - smoothstep(1.0f - max(tipFade, 1.0e-4f), 1.0f, along);
    const float ends   = muzzle * tip;

    // フェードが «切り口を隠す» のに対して、閃光は «そこで何かが起きている» を作る。
    // 銃口は噴き出す側、着弾点は潰れる側なので、同じ形でも役割は逆になる。
    const float muzzleBurst = pow(saturate(1.0f - along / max(muzzleFlare, 1.0e-4f)), 2.5f);
    const float tipBurst    = pow(saturate((along - 1.0f) / max(tipFlare, 1.0e-4f) + 1.0f), 2.5f);
    const float endBurst    = (muzzleBurst + tipBurst) * detailLayer;

    // ── 覗き込み ────────────────────────────────────────────────────────────
    // 軸と視線が揃うほど光路が長い。ビルボードが画面上で潰れる分をここで取り返す。
    const float3 axis  = normalize(input.axis);
    const float3 toEye = normalize(input.toEye);
    const float  glint = pow(saturate(abs(dot(axis, toEye))), 6.0f) * max(axialGlint, 0.0f);

    // ── 明滅 ────────────────────────────────────────────────────────────────
    // along を混ぜないと «一定周期で点滅するだけ» になるので、場所によってわずかに
    // ずらして «脈打っている» にする。
    const float flick = lerp(1.0f, 0.45f + 1.05f * ArcNoise(phase * 17.0f + along * 1.7f),
                             saturate(flicker));

    // ── 配色 ────────────────────────────────────────────────────────────────
    const float3 rail = albedo.rgb;
    // 白熱は «足す» のではなく «最も明るい成分へ揃える»。加算にすると芯が飽和して
    // ＋ と − が同じ白線になる (12.2)。揃えるだけなら明るさは変わらず彩度だけ落ちる。
    const float  peak = max(max(rail.r, rail.g), rail.b);
    const float3 hot  = lerp(rail, float3(peak, peak, peak), saturate(coreWhite));

    // rgb = «足す光»。芯と裾の比が線の細さそのものなので、両方に素材の輝度を掛ける。
    float3 emit = rail * glow * weave;
    emit += hot * core * max(coreBoost, 0.0f) * weave;
    emit += hot * braidLight * max(coreBoost, 0.0f) * 0.85f;
    // 壁は縞に依存させない。縞の暗い列で壁が欠けると «管が凹んだ» ように見える。
    emit += rail * shell * thickness3D;
    emit += rail * shockShape * max(ringBoost, 0.0f) * (1.0f + saturate(surge));
    // 粒は素材の縞に依存させない。縞の暗い列に来た粒が消えると数が合わなくなる。
    emit += lerp(rail, hot, 0.4f) * beadShape * 2.2f;
    emit += hot * endBurst * (1.0f + saturate(surge) * 1.5f);
    emit *= max(intensity, 0.0f) * ends * flick;
    // 覗き込みの増光は端のフェードより後に掛ける。先に掛けると、こちらを向いた線の
    // 切り口だけが四角く焼き付く。
    emit *= 1.0f + glint;

    // ── a = «隠す量» ────────────────────────────────────────────────────────
    // 芯だけが背景を隠し、縁は加算グローとして素通しにする。
    // WHY glow を少しだけ混ぜるか: 芯だけを隠す量にすると、芯の外側が完全な加算になり、
    //     明るい背景の上でビームが消える。わずかに隠しておくと、どんな背景でも線が残る。
    float occlusion = saturate(core
                             + braidLight * 0.8f
                             + shell.g * 0.35f
                             + glow * 0.25f
                             + beadShape * 0.5f
                             + shockShape * 0.3f)
                    * stripe.a * albedo.a * ends;
    // 太さのムラは明るさよりアルファに強く効かせる。明るさだけを落とすと、
    // 芯が細くならずに «灰色の帯» になって途切れて見えない。
    occlusion *= lerp(1.0f, thickness, saturate(crackle) * 0.75f);

    // 完全に何も足さない画素まで帯として描くと、透明なのに深度・ブレンドの帯域だけ食う。
    clip(max(occlusion, dot(emit, float3(1.0f, 1.0f, 1.0f))) - 0.002f);
    return float4(emit, occlusion);
}
