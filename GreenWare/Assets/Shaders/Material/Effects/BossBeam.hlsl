/// @file    BossBeam.hlsl
/// @brief   ボスのコアビーム。地面へ «エネルギーを捨てている» 切断機として描く。
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// PSO: SOLID_NOCULL + PREMULTIPLIED + DEPTH_READ
///
/// 頂点は BeamTrailRendererComponent が組む帯で、uv.x = アパーチャから接地点への進み
/// [0,1]、uv.y = 帯の横断 [0,1]。素材と合成の契約は Beam.hlsl (プレイヤー側) と同じ。
///
/// WHY プレイヤーの Beam.hlsl を流用しないか:
///   あちらは «撚られた電流» で、細い芯が何本も絡みながら流れる «生き物» の絵になっている。
///   6.4 が求めるのは «自分が出している線» の手応えなので、それで正しい。
///   ボスのコアビームは 8 章で «下面アパーチャから地面へ照射し、旋回して薙ぐ» と
///   決まっていて、プレイヤーがすべきことは «避けて動き続ける» こと。つまり読ませたいのは
///   «自分の出力» ではなく «逃げないと消される質量» で、必要な絵は真逆になる。
///   撚りを消して同心の焦点環に、粒を «落ちていく段» に置き換えてある。
///   両者が同じ電気に見えないのは意図した差で、揃えると «大きいだけの自分の銃» になる。
///
/// WHY 断面を «同心環» にするか:
///   撚りは筋の位置が毎フレーム変わるので «揺らいでいる» と読める。ボスの側でそれをやると
///   «不安定な出力» に見え、避ければ止まりそうな印象を与える。半径が固定の環を重ねると、
///   絞り切ったレンズの筒に見えて «この線は変わらない、動くのは自分» になる。
///
/// WHY 粒ではなく «段» が落ちるか:
///   プレイヤーの粒は等速で走る (銃口から着弾点へ電荷を «送っている»)。ボスは
///   地面へエネルギーを «捨てている» ので、接地点へ近づくほど速い方が読みに合う。
///   along を累乗してから位相へ入れるだけで、同じ 1 本の中で下ほど詰まって加速して見える。
///
/// WHY 点火 (charge) を持つか:
///   Beam_Start の 30F は «構え» のクリップで、その間ビームが «有る / 無い» の 2 値だと
///   1 フレームで全開の線が生える。針から本径まで太らせる 1 本のパラメーターがあれば、
///   照射前に «来る» が読める。8 章がすべての攻撃へ求める «明確な予兆» の一部。
///
/// WHY 接地点の膨らみをシェーダー側にも持つか:
///   接地の «点» そのものは VFX (火花と焦げ) が担当する。ただし線の側が先細りのままだと、
///   線と点が別の絵として並んでしまう。線の終端が広がって点へ受け渡されると、
///   1 つの出来事として繋がる。impactBloom はそのための «受け» で、
///   絵の主役を VFX から奪わない程度に留める。
///
/// WHY 企画書 12.2 のために芯を白へ振り «切らない» か:
///   Beam.hlsl と同じ理由。ボスの極は ＋ ⇄ − で切り替わり、プレイヤーはそれを読んで
///   逆極の雑魚をぶつける (8 章)。ビームが白飛びすると «今どちらか» が線から読めなくなる。
///   coreWhite は «最も明るい成分へ揃える» までで、明るさそのものは足さない。
///
/// WHY 事前乗算 (PREMULTIPLIED) か / WHY 時間を cbuffer から取らないか / WHY cbuffer の
/// 名前を変えてはいけないか:
///   すべて Beam.hlsl のヘッダーと同じ。とくに最後の 1 つは、名前が違うと
///   ShaderDescriptor の変数表が空のまま «有効» として返り、MaterialInstance::Set* が
///   per-instance の上書きを «全部黙って» 捨てる。
///
/// NOTE: 断面と流れの共通名 (coreWidth / tiling / phase …) は Beam.hlsl と揃えてある。
///       BeamTrailRendererComponent::PushMaterial がこの名前で書き込むため、帯を組む
///       仕組みをプレイヤーと共有できる。ボス固有の項目は BossBeamComponent が足す。

#include "Common/Binding.hlsli"

#define FBZZ_MATERIAL_CONSTANTS
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    /// 線の色と不透明度。LineRenderer が startColor から毎フレーム書き込む。
    /// rgb は極性色 × 明るさ (HDR)、a は «芯がどれだけ背景を隠すか»。
    float4 albedo;

    // ── 断面 (Beam.hlsl と共通の名前) ───────────────────────────────────────
    /// 芯 (bore) の太さ。帯の半幅に対する割合 [0,1]。
    float  coreWidth;
    /// 縁の減衰指数。大きいほど芯が細く鋭くなる。
    float  edgeFalloff;
    /// 芯の明るさ倍率。
    float  coreBoost;
    /// 全体の明るさ倍率 (HDR)。
    float  intensity;

    // ── 流れ ────────────────────────────────────────────────────────────────
    /// 素材を帯の長さ方向へ何回繰り返すか。0 で 1 枚を引き伸ばす。
    float  tiling;
    /// uv.x のスクロール量。スクリプトが毎フレーム進める。
    float  scroll;
    /// アパーチャ側の立ち上がり [0,1]。
    float  muzzleFade;
    /// 接地側の減衰 [0,1]。
    float  tipFade;

    // ── 乱れ ────────────────────────────────────────────────────────────────
    /// 乱れの位相。スクリプトが毎フレーム進める。
    float  phase;
    /// 芯を帯の中で振る幅 [0,1]。ボスでは «ほぼ振らない» のが既定。
    ///
    /// WHY 残すか: 0 にすると完全な直線になり、被弾したときに線が乱れる表現が作れない。
    ///     揺れるのは «削られたとき» だけ、という使い分けのために口だけ開けておく。
    float  arcAmp;
    /// 蛇行の空間周波数 [周/帯]。
    float  arcFreq;
    /// 芯の途切れ量 [0,1]。
    float  crackle;

    /// 帯全体の明滅の深さ [0,1]。
    float  flicker;
    /// 落ちる «段» の数。Beam.hlsl の粒に当たる枠を流用する。
    float  beadDensity;
    /// 段の頭の鋭さ。
    float  beadFalloff;
    /// この帯が芯層か裾層か。1 = 芯 / 0 = 裾。
    float  layer;

    /// 命中と点火の «張り» [0,1]。
    float  surge;

    // ── ボス固有 ────────────────────────────────────────────────────────────
    /// 点火 [0,1]。0 で針、1 で本径。太さ・明るさ・環の数がまとめてここに従う。
    ///
    /// WHY 1 本にまとめるか: «構えの間に太っていく» は 1 つの出来事で、太さと明るさと
    ///     環が別々のカーブで動くと、途中のどこかで «細いのに眩しい» 針が出る。
    float  charge;

    /// 同心環の本数 [0,4]。0 で環を出さない (中身の詰まった棒になる)。
    float  focusRings;
    /// いちばん外の環の半径 [半幅比]。内側の環はここを等分した位置に出る。
    float  focusRadius;
    /// 環の厚み。小さいほど硬い輪郭になる。
    float  focusWidth;
    /// 環の明るさ。
    float  focusBoost;
    /// 環の色収差 [半幅比]。R と B を逆へずらして縁へ色を割る。
    float  dispersion;

    /// 落ちる段が接地点へ向けて詰まる度合い。1 で等速、大きいほど下で加速する。
    ///
    /// WHY 累乗で表すか: 速度を «上と下で違う値» として持たせると、途中で不連続になるか
    ///     補間の式をもう 1 つ足すことになる。along を累乗してから位相へ入れれば、
    ///     1 つの数で «下ほど詰まる» が連続に出る。
    float  cascadeGravity;
    /// 段が落ちる速さ [周/位相]。位相の巻き戻りで飛ばないよう 1/2^n を選ぶ。
    float  cascadeSpeed;

    /// 接地側の膨らみが届く長さ [帯比]。0 で膨らませない。
    float  impactBloom;
    /// 膨らみが帯の外へどれだけ張り出すか [半幅比]。
    float  impactSpread;
    /// アパーチャ側の閃光が届く長さ [帯比]。
    float  muzzleFlare;

    /// 熱でぼやける外周の広さ [半幅比]。0 で出さない。
    ///
    /// WHY 芯の «縁» と別に持つか: 縁 (edgeFalloff) は線そのものの太さの一部で、
    ///     芯と同じ形をしている。熱は線から離れたところで空気が光る現象なので、
    ///     線の形からはみ出していないと «熱» に見えず、ただの太い線になる。
    float  heatWash;
    /// 熱のにじみの明るさ。
    float  heatBoost;

    /// 円柱として厚みを積む量 [0,1]。0 で平らな帯。
    float  volume;
    /// 2 枚目の縞の効き [0,1]。
    float  detail;
    /// 芯を白熱へ寄せる量 [0,1]。1 でも彩度が落ちるだけで明るさは増えない。
    float  coreWhite;
    /// 線を «覗き込んだ» ときの増光。
    float  axialGlint;

    /// 筒の半径 [m]。0 なら «板» として uv から断面を作る (従来の経路)。
    ///
    /// WHY 半径をシェーダーが知る必要があるか:
    ///   筒ジオメトリになると、画素が居るのは «筒の表面» で、断面のどこかではない。
    ///   uv から半径を作る従来の式は «板を円柱に見立てる» ための近似で、軸方向から
    ///   覗いた瞬間に破綻する (板が画面上で潰れるのに厚みだけ残る)。実寸の半径が
    ///   あれば視線と円柱の交差を解けるので、どの角度から見ても厚みが正しく出る。
    float  tubeRadius;
    float  _bossBeamPad0;
};

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Material/Effects/ArcNoise.hlsli"
#include "Platform/Backend.hlsli"

Texture2D    texAlbedo   : register(TEX_ALBEDO);
SamplerState sampDefault : register(SAMPLER_DEFAULT);

struct BossBeamPSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
    /// 帯の進行方向 (ワールド)。LineRenderer が区間の向きを tangent へ入れている。
    float3 axis       : TEXCOORD1;
    /// 画素からカメラへ向かうベクトル (ワールド、非正規化)。
    float3 toEye      : TEXCOORD2;
    /// この画素のワールド位置。筒の断面を解くのに要る。
    float3 worldPos   : TEXCOORD3;
    /// 筒の外向き法線 (ワールド)。表面から軸までを戻すのに使う。
    float3 outward    : TEXCOORD4;
};

/// 視線と円柱の交差。
///   outRadial    … 視線が軸へ最も近づいたときの距離 / 半径 [0,1]
///   outThickness … 視線が筒を貫く長さ / 直径 [0,1]
///
/// WHY 弦の長さをそのまま «厚み» として使えるか:
///   発光する煙のような媒質を «視線が通った距離だけ光る» と近似すると、積分は
///   光路長そのものになる。円柱なら光路長は弦の長さなので、閉じた式で解ける。
///   板の近似 (sqrt(1 - r^2)) は «視線が軸に垂直» を仮定した特殊解でしかない。
void BossBeamCylinder(float3 worldPos, float3 outwardNormal, float3 axisDir, float radius,
                      out float outRadial, out float outThickness)
{
    outRadial    = 1.0f;
    outThickness = 0.0f;
    if (radius <= 1.0e-4f) return;

    // 表面の画素から半径ぶん戻れば軸上の点。
    const float3 axisPoint = worldPos - outwardNormal * radius;
    const float3 rayOrigin = cameraPos;
    const float3 rayDir    = normalize(worldPos - cameraPos);

    // 軸に垂直な平面へ落として、2 次元の円と直線の交差にする。
    const float3 perpDir = rayDir - axisDir * dot(rayDir, axisDir);
    const float3 offset  = (rayOrigin - axisPoint) - axisDir * dot(rayOrigin - axisPoint, axisDir);

    const float a = dot(perpDir, perpDir);
    // 軸をほぼ真正面から覗いている。円との交差が解けないので «最大の厚み» に倒す。
    // WHY 0 ではなく 1 か: 軸方向は光路が最も長い。潰れて消えるのは板の都合であって、
    //     筒として正しいのは «焼き付く» 側。
    if (a < 1.0e-6f) { outRadial = 0.0f; outThickness = 1.0f; return; }

    const float b = 2.0f * dot(offset, perpDir);
    const float c = dot(offset, offset) - radius * radius;
    const float discriminant = b * b - 4.0f * a * c;
    if (discriminant <= 0.0f) return;   // 掠っただけ。厚みは 0。

    outThickness = saturate(sqrt(discriminant) / a / (2.0f * radius));

    // 最接近点までの距離。そこでの軸からの距離が断面の «半径» になる。
    const float closest = -b / (2.0f * a);
    outRadial = saturate(length(offset + perpDir * closest) / radius);
}

/// ガウス環 1 本。色収差は半径をずらして 3 回呼ぶ。
float BossBeamRing(float radius, float ringRadius, float width)
{
    const float offset = (radius - ringRadius) / max(width, 1.0e-3f);
    return exp(-offset * offset);
}

/// 同心環の合計。環は «外から内へ» 等間隔に置く。
///
/// WHY 内側ほど細くするか:
///   等幅で並べると内側の環が芯に呑まれて «太い芯» に戻る。内へ行くほど絞ると、
///   芯・内環・外環が別々の層として読め、筒が «絞り込まれている» ように見える。
float BossBeamFocus(float radius, float count, float outerRadius, float width)
{
    float total = 0.0f;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        // WHY break ではなく重みで消すか: 本数は per-instance で毎フレーム変わりうる。
        //     分岐で抜くと隣接画素で経路が割れる。掛け算なら常に同じ命令列で済む。
        const float active = step((float)i + 0.5f, count);
        const float lane   = ((float)i + 1.0f) / max(count, 1.0f);
        total += BossBeamRing(radius, outerRadius * lane, width * lane) * active;
    }
    return total;
}

BossBeamPSIn VSMain(VSInput v)
{
    BossBeamPSIn o;
    float4 worldPos = mul(float4(v.position, 1.0f), world);
    o.svPosition = mul(worldPos, viewProjection);
    o.uv       = v.uv;
    o.axis     = mul(v.tangent, (float3x3)world);
    o.toEye    = cameraPos - worldPos.xyz;
    o.worldPos = worldPos.xyz;
    // 板のときは法線に意味が無い (LineRenderer が定数を入れている)。筒のときだけ使う。
    o.outward  = mul(v.normal, (float3x3)world);
    return o;
}

float4 PSMain(BossBeamPSIn input) : SV_Target0
{
    const float along = saturate(input.uv.x);
    // 芯だけが持つ要素の重み。裾層 (layer = 0) では丸ごと消える。
    const float detailLayer = saturate(layer);
    // 点火。太さも明るさも環の数もここから出る。
    const float ignite = saturate(charge);
    const bool  isTube = tubeRadius > 1.0e-4f;

    // ── 断面 ────────────────────────────────────────────────────────────────
    // 筒: 視線と円柱を実際に交差させる。板: uv を円柱に見立てた従来の近似。
    float radius   = 1.0f;
    float centered = 0.0f;
    float chord    = 0.0f;
    if (isTube) {
        BossBeamCylinder(input.worldPos, normalize(input.outward), normalize(input.axis),
                         tubeRadius, radius, chord);
        // 筒には «帯のどちら側か» が無い。符号は蛇行にしか使わないので、
        // 半径をそのまま渡して «芯の太さが揺らぐ» 側で受ける。
        centered = radius;
    } else {
        // 帯の中心を 0 とした符号付きの横断座標。芯を «ずらす» ので符号を残す。
        centered = (input.uv.y - 0.5f) * 2.0f;
        radius   = saturate(abs(centered));
        chord    = sqrt(saturate(1.0f - radius * radius));
    }
    const float thickness3D = lerp(1.0f, chord, saturate(volume));

    // 筒は表と裏の 2 面が同じ画素を通る (LineRenderer が両面描画を強制するため)。
    // 同じ視線なので弦の長さは 2 回とも同じ値になり、そのまま足すと厚みが倍になる。
    // 半分ずつ載せれば合計がちょうど 1 本ぶんの光路長になる。
    //
    // WHY 片面描画にしないか: 巻き順の前後を «描画側の都合» としてシェーダーが
    //     仮定することになる。両面のまま半分ずつ載せる方が、剥がれたときに
    //     «倍明るい» で済み、«消える» にならない。
    const float faceShare = isTube ? 0.5f : 1.0f;

    // ── 接地側の膨らみ ──────────────────────────────────────────────────────
    // 終端へ近づくほど «帯の実効半幅» を広げる。半径を割るだけなので、芯も環も熱も
    // まとめて広がる (要素ごとに «終端では広い» を書かなくて済む)。
    float flare = 1.0f;
    if (impactBloom > 0.0f) {
        const float nearTip = saturate((along - (1.0f - impactBloom)) / max(impactBloom, 1.0e-4f));
        flare = 1.0f + nearTip * nearTip * max(impactSpread, 0.0f) * ignite;
    }
    const float across0 = radius / flare;

    // ── 蛇行 ────────────────────────────────────────────────────────────────
    // 既定ではほぼ振らない。振れるのは «削られたとき» だけ、という使い分け。
    const float headroom = saturate(1.0f - coreWidth);
    const float swing = arcAmp * headroom * (0.25f + 0.75f * along);
    const float snake = (ArcFbm(along * max(arcFreq, 0.0f) + phase * 2.3f) - 0.5f) * 2.0f * swing;
    const float across = saturate(abs(centered - snake) / flare);

    // ── 縞: 倍率と向きの違う 2 枚 ───────────────────────────────────────────
    // 素材は無彩色 (RGB = 輝度 / A = カバレッジ)。色は albedo から乗る。
    const float tiles    = max(tiling, 0.0f);
    const float baseLane = (tiles > 0.0f ? along * tiles : along);
    const float4 stripe  = texAlbedo.Sample(sampDefault, float2(baseLane + scroll, input.uv.y));
    // 2 枚目は «粗く・逆向き»。scroll は 1024 で巻き取られるので、係数は 1/4 刻み。
    const float4 counter = texAlbedo.Sample(sampDefault,
                                            float2(baseLane * 0.25f - scroll * 1.25f,
                                                   input.uv.y * 0.5f + 0.25f));
    const float3 weave = stripe.rgb * lerp(1.0f, 0.55f + 0.9f * counter.r, saturate(detail));

    // ── 芯 (bore) ───────────────────────────────────────────────────────────
    // 点火中は針。太さそのものを ignite で絞るので、細い間は明るくても «細い»。
    const float grain     = ArcFbm(along * 18.0f + phase * 4.1f);
    const float thickness = lerp(1.0f, grain, saturate(crackle));
    const float bore      = max(coreWidth * thickness * lerp(0.12f, 1.0f, ignite), 1.0e-4f);
    // WHY smoothstep の幅を芯の 25% に絞るか: プレイヤー側は 0 から芯幅まで滑らかに
    //     落とすので «光の束» になる。こちらは縁を立てて «切っている棒» にする。
    const float coreAA = max(fwidth(across), 1.0e-4f);
    const float core = (1.0f - smoothstep(max(bore - coreAA, 0.0f), bore + coreAA, across))
                    * saturate(bore / coreAA);

    // ── 焦点環 ──────────────────────────────────────────────────────────────
    // R と B を逆へずらして縁へ色を割る。中央 (G) は必ず本来の半径で採るので、
    // 色収差を上げても環の «位置» そのものは動かない。
    const float ringCount  = clamp(focusRings, 0.0f, 4.0f) * ignite;
    const float ringRadius = focusRadius * lerp(0.35f, 1.0f, ignite);
    const float3 rings = float3(BossBeamFocus(across0 + dispersion, ringCount, ringRadius, focusWidth),
                                BossBeamFocus(across0,              ringCount, ringRadius, focusWidth),
                                BossBeamFocus(across0 - dispersion, ringCount, ringRadius, focusWidth))
                       * focusBoost * detailLayer;

    // ── 裾 ──────────────────────────────────────────────────────────────────
    const float glow = pow(saturate(1.0f - across), max(edgeFalloff, 0.01f)) * thickness3D;

    // ── 熱のにじみ ──────────────────────────────────────────────────────────
    // 線の «外» で空気が光る。帯の形からはみ出していないと熱に見えないので、
    // 半幅そのものを基準にした裾とは別の減衰で作る。
    float wash = 0.0f;
    if (heatWash > 0.0f) {
        wash = exp(-across0 * across0 / max(heatWash * heatWash, 1.0e-4f));
        // 接地側ほど濃い。地面へ当たっている所がいちばん熱い、という当たり前を出す。
        wash *= (0.35f + 0.65f * along) * ignite;
    }

    // ── 落ちる段 ────────────────────────────────────────────────────────────
    // along を累乗してから位相へ入れる。下ほど間隔が詰まり、同じ 1 本の中で
    // «加速しながら落ちている» が出る。
    float cascade = 0.0f;
    if (beadDensity > 0.0f) {
        const float fall = pow(along, max(cascadeGravity, 0.01f));
        const float lane = fall * beadDensity + phase * cascadeSpeed;
        // 頭を鋭く、尾を上へ引く。左右対称だと «光っている印» にしかならず、
        // どちらへ落ちているのかが段そのものから読めない。
        const float behind = saturate(1.0f - frac(lane));
        cascade = pow(behind, max(beadFalloff, 1.0f))
                + pow(behind, max(beadFalloff, 1.0f) * 0.2f) * 0.3f;
    }
    // 断面にも乗せる。帯いっぱいに光らせると «帯が明滅している» になって段に見えない。
    const float cascadeShape = cascade * pow(saturate(1.0f - across0), 1.4f) * detailLayer * ignite;

    // ── 端 ──────────────────────────────────────────────────────────────────
    const float muzzle = smoothstep(0.0f, max(muzzleFade, 1.0e-4f), along);
    const float tip    = 1.0f - smoothstep(1.0f - max(tipFade, 1.0e-4f), 1.0f, along);
    const float ends   = muzzle * tip;

    // アパーチャは «噴き出す側»。接地側の閃光は VFX が主役なので、ここでは出さない
    // (線が自分で光ると、火花より線の方が明るい «点» になって主客が入れ替わる)。
    const float apertureBurst =
        pow(saturate(1.0f - along / max(muzzleFlare, 1.0e-4f)), 2.5f) * detailLayer * ignite;

    // ── 覗き込み ────────────────────────────────────────────────────────────
    const float3 axis  = normalize(input.axis);
    const float3 toEye = normalize(input.toEye);
    const float  glint = pow(saturate(abs(dot(axis, toEye))), 6.0f) * max(axialGlint, 0.0f);

    // ── 明滅 ────────────────────────────────────────────────────────────────
    // along を混ぜて «一定周期の点滅» にしない。
    const float flick = lerp(1.0f, 0.55f + 0.9f * ArcNoise(phase * 11.0f + along * 1.3f),
                             saturate(flicker));

    // ── 配色 ────────────────────────────────────────────────────────────────
    const float3 rail = albedo.rgb;
    // 白熱は «足す» のではなく «最も明るい成分へ揃える» (12.2)。
    const float  peak = max(max(rail.r, rail.g), rail.b);
    const float3 hot  = lerp(rail, float3(peak, peak, peak), saturate(coreWhite));

    // rgb = «足す光»。
    float3 emit = rail * glow * weave;
    emit += hot * core * max(coreBoost, 0.0f) * weave;
    // 環と熱は縞に依存させない。縞の暗い列で環が欠けると «筒が凹んだ» ように見える。
    emit += rail * rings * thickness3D;
    emit += rail * wash * max(heatBoost, 0.0f);
    emit += lerp(rail, hot, 0.45f) * cascadeShape * 2.0f;
    emit += hot * apertureBurst * (1.0f + saturate(surge) * 1.5f);
    emit *= max(intensity, 0.0f) * ends * flick * lerp(0.25f, 1.0f, ignite) * faceShare;
    // 覗き込みの増光は端のフェードより後。先に掛けると切り口だけが四角く焼き付く。
    emit *= 1.0f + glint;

    // ── a = «隠す量» ────────────────────────────────────────────────────────
    // 芯と環だけが背景を隠す。熱のにじみは «空気が光っている» だけなので純加算に残す
    // (隠すと線の周りが曇り、地面の焦げが読めなくなる)。
    float occlusion = saturate(core
                             + rings.g * 0.45f
                             + glow * 0.22f
                             + cascadeShape * 0.4f)
                    * stripe.a * albedo.a * ends * ignite;
    // 太さのムラは明るさよりアルファに強く効かせる。
    occlusion *= lerp(1.0f, thickness, saturate(crackle) * 0.75f);
    // 隠す量も 2 面ぶんに割る。前後 2 回の合成で 1 - (1-a)(1-a) となり、元の a へ近づく。
    occlusion *= faceShare;

    // 何も足さない画素まで帯として描くと、透明なのに深度・ブレンドの帯域だけ食う。
    clip(max(occlusion, dot(emit, float3(1.0f, 1.0f, 1.0f))) - 0.002f);
    return float4(emit, occlusion);
}
