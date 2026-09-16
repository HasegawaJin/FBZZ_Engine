/// @file    WeaponTrail.hlsl
/// @brief   刃が掃いた面そのものを «絹の帯 + 白熱した刃先» として描く
/// @author  Hasegawa Jin
/// @date    2026-09-06
///
/// PSO: SOLID_NOCULL + PREMULTIPLIED + DEPTH_READ
///
/// 頂点は BladeTrailComponent が «刀の 2 つのソケットが通った跡» から張る帯で、
///   uv.x = 経過 [0,1] … 0 が «今の刃»、1 が寿命の端
///   uv.y = 刃の横断 [0,1] … 0 が鍔、1 が切っ先
/// 頂点カラーは層の色味 (rgb) と重み (a)。
///
/// WHY 極性を持ち込まないか:
///   剣の軌跡は «どちらの剣を振ったか» ではなく «斬った» を伝える層。赤青を乗せると
///   盤面 (敵の極) と同じ色語彙を毎振り画面いっぱいに撒くことになり、極の情報が
///   軌跡のノイズに埋もれる。極は自分の纏いと環が受け持つ (2026-09-06 に極性色の弧
///   旧 Slash シェーダーを廃したのはこれが理由)。
///
/// WHY 時計を cbuffer で渡さないか (廃した弧との最大の違い):
///   あちらは «1 度張った弧» の上を光が走るので、進みは全頂点で共通の 1 つの数。
///   こちらは頂点そのものが «その時刻に刃が居た場所» なので、経過は頂点ごとに違う。
///   uv.x に焼いてしまえば、帯の途中で刃が加速しても、その加速が帯の詰まり方として
///   そのまま出る ─ 均等割りの uv だと «速い振りも遅い振りも同じ模様» になる。
///
/// WHY 視線に対する «立ち» で明るさを持ち上げるか (rim):
///   この帯は 3D の掃過面なので、面を真横から見ると投影面積が 0 になる。真面目に
///   陰影を付けるほど «振ったのに何も出ない角度» が生まれる。掠める角ほど光を足せば、
///   面が立っている間は細く鋭い線として残り、寝ている間は面として広がる ─
///   どちらの角度でも «速い金属の面» として読める。
///
/// WHY 消し方を «筋の抜け» にするか:
///   一様に薄くすると、帯は形を保ったまま暗くなって最後に消える。刃が空気を裂いた跡は
///   «筋が 1 本ずつ抜けていく» ので、繊維ごとに寿命を散らす。消えている最中も形が
///   動き続け、«まだ何かが起きている» ままフェードアウトできる。
///
/// WHY cbuffer の名前を変えてはいけないか:
///   ShaderDescriptor は PS バイトコードを GetConstantBufferByName("MaterialConstants")
///   で引いて変数表を作る。名前が違うと表が空のまま «有効な Descriptor» が返り、
///   MaterialInstance::Set* が per-instance 上書きを全部黙って捨てる。

#include "Common/Binding.hlsli"

#define FBZZ_MATERIAL_CONSTANTS
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    /// 白熱の色 (HDR)。刃先と刃が «今» 居る位置がこの色になる。
    float4 coreColor;
    /// 帯の主色 (HDR)。金の軌跡はここが持つ。
    float4 edgeColor;
    /// 外へ抜けていく縁の色 (HDR)。芯と補色寄りにすると «厚み» が出る。
    float4 fringeColor;

    /// 乱れの位相。BladeTrailComponent が毎フレーム進める (小さい値で巻き取る)。
    float  phase;
    /// 一振りの «格» [0,1]。締めと溜め斬りで上がる。色温度と明るさに効く。
    float  heat;
    /// 命中の «張り» [0,1]。当たった一撃だけ立てて減衰させる (BladeTrailComponent)。
    ///
    /// WHY heat と分けるか: 格は振り出しに決まる «その一撃がどれだけ大きいか» で、
    ///   命中は判定が出た後に決まる «届いたか»。1 つに畳むと、空を斬った締めと
    ///   当たった 1 段目が同じ絵になる ─ どちらの情報も «強さ» に潰れる。
    float  flash;
    /// 全体の明るさ倍率 (HDR)。
    float  intensity;
    /// 芯がどれだけ背景を隠すか [0,1]。0 で純粋な加算グローになる。
    float  opacity;

    /// 経過に対する減衰の指数。大きいほど «短く鋭い» 軌跡になる。
    float  lifeFalloff;
    /// 刃の直後に残る白熱の長さ [経過比]。
    float  headLength;
    /// その明るさ。0 で出さない。
    float  headBoost;
    /// 帯そのものの明るさ。
    float  bodyGain;

    /// 鍔から切っ先への明るさの傾き。大きいほど «切っ先だけが光る» に寄る。
    float  bodyFalloff;
    /// 白熱の筋を横断のどこへ置くか [0,1]。1 に近いほど切っ先の側。
    float  railBias;
    /// その幅 [半幅比]。細いほど鋭い線になる。
    float  railWidth;
    /// その明るさ。帯との差が軌跡の «細さ» を決める。
    float  railBoost;

    /// 筋の色収差 [半幅比]。R と B を逆向きにずらして縁へ色を割る。
    float  dispersion;
    /// 鍔側の立ち上がり [横断比]。0 で切り口がそのまま出る。
    float  rootFade;
    /// 切っ先側の減衰 [横断比]。帯の «外» へ抜ける柔らかさ。
    float  tipFade;

    /// 内側の弧 (鍔の側) の縁の幅 [横断比]。位置は rootFade がそのまま決める。
    float  spineWidth;
    /// その明るさ。0 で内側は «滲んで消える» だけになる。
    float  spineBoost;

    // ── 節 ──────────────────────────────────────────────────────────────────
    // WHY 要るか («のっぺり» の構造的な原因その 2):
    //   これが無いと、帯の色は横断方向 (鍔 → 切っ先) の 1 本のグラデーションでしか
    //   変わらない。掃過方向へは減衰しかないので、静止画にすると «ただの帯» になる。
    //   人が «速い» と読むのは明るさではなく «模様が流れていること» なので、
    //   掃過方向を刻んで流す層が要る。
    /// 帯を横切る節の数 [本/帯]。0 で出さない。
    float  bandCount;
    /// 節が流れる速さ [周/位相]。位相の巻き取り (64) で飛ばないよう 1/2^n を選ぶ。
    float  bandSpeed;
    /// 節の傾き [周/横断]。0 で帯に直交、上げるほど «く» の字に寝る。
    float  bandSkew;
    /// 節の鋭さ。大きいほど細い線、小さいほど緩い濃淡になる。
    float  bandSharp;
    /// 節が帯を «削る» 深さ [0,1]。0 で明るさだけ足す (削らない)。
    float  bandDepth;
    /// 節そのものの明るさ。
    float  bandBoost;

    /// 絹の繊維の本数 [本/帯幅]。0 で出さない。
    float  streakDensity;
    /// 繊維の細さ。大きいほど鋭い線になる。
    float  streakSharp;
    /// 繊維の明るさ。
    float  streakGain;
    /// 繊維が横へ流れる速さ [幅/位相]。0 で流れない。
    float  streakDrift;

    /// 掠める角で持ち上げる指数。大きいほど «真横から見たときだけ» に効く。
    float  rimPower;
    /// その量。0 で 3D の掃過面がそのまま出る (真横で消える)。
    float  rimBoost;
    /// 抜けの細かさ [周/帯]。小さいと «大きな塊»、大きいと «砂» で抜ける。
    float  dissolveScale;
    /// 抜けの縁の柔らかさ。0 に近いほど «割れる»、大きいほど «溶ける»。
    float  dissolveSoft;

    /// 抜けを鍔側へ寄せる指数。切っ先の筋は最後まで残す。
    float  dissolveBias;
    /// 刃先で弾ける火花の密度 [個/帯]。0 で出さない。
    float  sparkDensity;
    /// 同時に点いている火花の割合 [0,1]。
    float  sparkRate;
    /// 火花の明るさ。
    float  sparkBoost;

    /// 切り分け用の表示モード。0 = 通常。
    ///   1 … 帯を単色で塗り潰す。模様を一切通さないので、ここで縞や途切れが見えたら
    ///        原因は «帯の張り方 (幾何) / UV / ブレンド» のどれか。
    ///   2 … uv をそのまま色にする。R = 経過 / G = 刃の横断。
    ///        R が 0 から 1 へ一度だけ登り、G が鍔から切っ先へ一度だけ登れば UV は正しい。
    ///   3 … 抜けを含めた «在る量» を白で出す。寿命の確認用。
    float  debugMode;
};

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"

struct TrailPSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
    /// 掃過面の法線 (ワールド)。掠める角の判定にだけ使う。
    float3 normalWS   : TEXCOORD1;
    /// 画素からカメラへ (正規化前)。
    float3 toEye      : TEXCOORD2;
    float4 color      : COLOR;
};

// WHY frac(sin(...)) を使わないか:
//   sin の引数が大きくなると float32 の引数簡約で下位ビットが落ち、戻り値が «乱数»
//   ではなく規則的な数列になる。ここは繊維番号 × 大きな係数を渡すのですぐその領域へ
//   入り、潰れたハッシュはノイズではなく縞を作る (廃した Slash シェーダーが踏んだのと同じ轍)。
float TrailHash2(float2 cell)
{
    float3 p = frac(float3(cell.x, cell.y, cell.x) * float3(0.1031f, 0.1030f, 0.0973f));
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

float TrailNoise2(float2 p)
{
    const float2 cell = floor(p);
    const float2 f    = frac(p);
    const float2 w    = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(TrailHash2(cell),                      TrailHash2(cell + float2(1.0f, 0.0f)), w.x),
                lerp(TrailHash2(cell + float2(0.0f, 1.0f)), TrailHash2(cell + float2(1.0f, 1.0f)), w.x),
                w.y);
}

/// 3 オクターブ。オフセットを足すのは、同じ格子が重なって縞が戻るのを防ぐため。
float TrailFbm2(float2 p)
{
    return TrailNoise2(p) * 0.55f
         + TrailNoise2(p * 2.17f + 13.7f) * 0.30f
         + TrailNoise2(p * 4.63f + 41.3f) * 0.15f;
}

/// 筋を 1 本のガウスで作る。色収差は横断位置をずらして 3 回呼ぶ。
float TrailRail(float lateral)
{
    // 遠景の細線を1画素未満で点滅させず、広げた幅の分だけ光量を戻す。
    const float width = max(railWidth, 1.0e-3f);
    const float filtered = max(width, fwidth(lateral) * 0.65f);
    const float d = lateral / filtered;
    return exp(-d * d) * width / filtered;
}

TrailPSIn VSMain(VSInputColor v)
{
    TrailPSIn o;
    const float4 worldPos = mul(float4(v.position, 1.0f), world);
    o.svPosition = mul(worldPos, viewProjection);
    o.uv         = v.uv;
    // 掃過面は毎フレーム組み直す薄い帯で、world は単位行列。それでも法線を素通しに
    // しないのは、帯を親の下へ移した瞬間に «掠める角» だけが静かに壊れるため。
    o.normalWS   = mul(v.normal, (float3x3)worldInvTranspose);
    o.toEye      = cameraPos - worldPos.xyz;
    o.color      = v.color;
    return o;
}

float4 PSMain(TrailPSIn input) : SV_Target0
{
    const float age    = saturate(input.uv.x);
    const float across = saturate(input.uv.y);

    if (debugMode > 0.5f) {
        if (debugMode < 1.5f) return float4(edgeColor.rgb * 0.6f, 1.0f);
        if (debugMode < 2.5f) return float4(age, across, 0.0f, 1.0f);
    }

    // ── 経過 ────────────────────────────────────────────────────────────────
    // 頂点ごとに焼かれた経過をそのまま減衰へ通す。均等割りの uv ではないので、
    // 刃が速く動いた区間は帯が伸び、そのぶん «薄く長く» なる。
    const float fade = pow(saturate(1.0f - age), max(lifeFalloff, 0.01f));

    // ── 刃の横断 ────────────────────────────────────────────────────────────
    // 切っ先へ向かって明るくする。刃は根元より先端の方が速く長い距離を掃くので、
    // 一様に塗ると «同じ幅の板» になって刃の面として読めない。
    const float root = smoothstep(0.0f, max(rootFade, 1.0e-4f), across);
    const float tip  = 1.0f - smoothstep(1.0f - max(tipFade, 1.0e-4f), 1.0f, across);
    const float ends = root * tip;
    float body = pow(across, max(bodyFalloff, 0.01f));

    // ── 節 ──────────────────────────────────────────────────────────────────
    // 掃過方向へ流れる刻み。across でずらすので «く» の字に寝て、刃が空気を
    // 押しのけた跡として読める (帯に直交させると «梯子» になって模様に見える)。
    //
    // WHY 三角波か: sin は丸いので «節» ではなく «濃淡» にしかならない。
    //   角があって初めて、流れているものが «連なった塊» として分離して見える。
    float chevron = 0.0f;
    if (bandCount > 0.0f) {
        const float lane = age * bandCount - phase * bandSpeed + across * bandSkew;
        const float saw  = abs(frac(lane) * 2.0f - 1.0f);
        chevron = pow(saturate(1.0f - saw), max(bandSharp, 0.01f));
        // 節は切っ先の側ほど強い。鍔の側まで刻むと、痩せた尾が虫食いに見える。
        chevron *= pow(across, 1.6f);
        // 削るのは «帯» だけ。この後の筋 (rail) には掛けないので、刻んでも
        // 刃の線は 1 本に繋がったまま残る。
        body *= lerp(1.0f - saturate(bandDepth), 1.0f, chevron);
    }

    // 白熱の筋。切っ先寄りに置いた «刃そのもの» の線。
    // 外側と内側で残りの幅が違うので、それぞれの側の幅で割って [-1,1] へ正規化する。
    // こうしないと bias が端へ寄った瞬間、狭い側だけ筋が帯からはみ出して四角く切れる。
    const float safeBias = clamp(railBias, 0.05f, 0.95f);
    const float offset   = across - safeBias;
    const float span     = offset > 0.0f ? (1.0f - safeBias) : safeBias;
    const float lateral  = offset / max(span, 1.0e-3f);
    const float3 rail = float3(TrailRail(lateral + dispersion),
                               TrailRail(lateral),
                               TrailRail(lateral - dispersion));

    // ── 内側の弧 ────────────────────────────────────────────────────────────
    // WHY 縁を 2 本持つか:
    //   帯は «切っ先の弧» と «鍔の弧» の 2 本に挟まれた面で、三日月の形はこの 2 本が
    //   決める。内側を滲ませたまま (bodyFalloff の裾) にすると、外側の弧だけが浮いた
    //   «光る紐» になって、面としての厚みが読めない。BladeTrailComponent が古い側を
    //   鍔から詰めるようになったぶん、この内側の弧は «跡が今どこまで痩せたか» を
    //   そのまま描く線でもある。
    //
    // WHY 位置を rootFade と共有するか:
    //   帯が «始まる» 場所は 1 つしかない。別の数で持つと、片方を触るたびに縁が
    //   切り口から離れて «帯の内側に線が 1 本浮いている» 絵になる。
    const float spineAt = max(rootFade, 1.0e-3f);
    const float spineD  = (across - spineAt) / max(spineWidth, 1.0e-3f);
    const float spine   = exp(-spineD * spineD);

    // ── 刃が «今» 居る場所 ──────────────────────────────────────────────────
    // 帯の頭は刀身そのものに重なっている。ここを白熱させると、光が刃から
    // «生えている» ように繋がる。切り離すと帯が刀の後ろに浮いて見える。
    const float head = exp(-age / max(headLength, 1.0e-3f));

    // ── 絹の繊維 ────────────────────────────────────────────────────────────
    // 帯を «1 枚の面» のままにすると、どれだけ明るくしても板にしか見えない。
    // 刃の横断方向に細い筋を刻み、筋ごとに寿命を散らすと、面が «速く流れている»
    // という読みに変わる。抜け (dissolve) と別に持つのは、こちらが «質» で
    // あちらが «消え方» だから ─ 混ぜると寿命を触るたびに質感まで動く。
    float streak = 0.0f;
    if (streakDensity > 0.0f) {
        const float lane  = across * streakDensity + phase * streakDrift;
        const float cell  = floor(lane);
        const float seed  = TrailHash2(float2(cell, 3.7f));
        // 繊維ごとの «長さ»。短い筋が先に抜け、長い筋が切っ先の側に残る。
        const float span0 = lerp(0.35f, 1.0f, seed);
        const float alive = 1.0f - smoothstep(span0 * 0.55f, span0, age);
        // 筋の中心からの距離。中心をずらすのは、等間隔の縞に見せないため。
        const float at    = lerp(0.25f, 0.75f, TrailHash2(float2(cell + 19.0f, 8.1f)));
        const float w     = 1.0f - abs(frac(lane) - at) * 2.0f;
        streak = pow(saturate(w), max(streakSharp, 1.0f)) * alive
               * lerp(0.45f, 1.0f, TrailHash2(float2(cell + 41.0f, 2.3f)));
    }

    // ── 火花 ────────────────────────────────────────────────────────────────
    // 刃先の外で弾ける短い光。位相を量子化した種で引くのは、連続時間で動かすと
    // «滑らかに明滅する» になって、弾ける不連続さが出ないため。
    float sparks = 0.0f;
    if (sparkDensity > 0.0f) {
        const float strobe = floor(phase * 2.5f);
        const float cell   = floor(age * sparkDensity);
        const float on     = step(1.0f - saturate(sparkRate), TrailHash2(float2(cell, strobe)));
        const float at     = TrailHash2(float2(cell + 37.0f, strobe + 11.0f));
        const float dot0   = 1.0f - abs(frac(age * sparkDensity) - at) * 2.0f;
        // 切っ先の側へ寄せる。中央で光らせると帯に埋もれて «太い線» にしかならない。
        const float rim    = pow(saturate(across), 3.0f);
        sparks = pow(saturate(dot0), 24.0f) * on * rim;
    }

    // ── 抜け ────────────────────────────────────────────────────────────────
    // 鍔側から先に千切れる。切っ先の筋まで同時に抜くと、軌跡が «一斉に消えた» に
    // なって、刃が通った向きが最後まで残らない。
    const float noise = TrailFbm2(float2(age * max(dissolveScale, 0.0f) + phase * 0.2f,
                                         across * 2.3f));
    const float soft  = max(dissolveSoft, 1.0e-3f);
    const float cut   = age * lerp(0.35f, 1.0f, pow(saturate(1.0f - across),
                                                    max(dissolveBias, 0.01f)));
    const float tatter = 1.0f - smoothstep(noise - soft, noise + soft, cut);

    const float tailWidth = lerp(0.0f, 0.55f, smoothstep(0.2f, 1.0f, age));
    const float tail = smoothstep(tailWidth, tailWidth + 0.22f, across);
    const float presence = fade * ends * tatter * tail;
    if (debugMode > 2.5f) return float4(presence.xxx, presence);

    // ── 掠める角 ────────────────────────────────────────────────────────────
    // 面が寝ているほど広く薄く、立っているほど細く強く。3D の掃過面を «消えない»
    // ままにするのはこの項だけなので、0 にすると真横で軌跡が丸ごと落ちる。
    const float3 normalWS = normalize(input.normalWS);
    const float3 toEye    = normalize(input.toEye);
    const float  grazing  = pow(saturate(1.0f - abs(dot(normalWS, toEye))),
                                max(rimPower, 0.01f));
    const float  rimGain  = 1.0f + max(rimBoost, 0.0f) * grazing;

    // ── 色 ──────────────────────────────────────────────────────────────────
    // 縁 → 主色 → 白熱、を 1 本の «温度» で辿る。層ごとに色を持たせると、
    // 明るさを触るたびに «どの層の色が変わったのか» が絵から切り分けられなくなる。
    // 命中は帯の «頭寄り» を白熱させる。全体へ一様に乗せると «当たった» ではなく
    // «材質が明るくなった» に見えて、どこで斬れたのかが出ない ─ 刃が触れたのは
    // 判定が出た «その瞬間の位置» で、それは経過 0 のあたりに写っている。
    const float strike = saturate(flash) * exp(-age / max(headLength * 2.4f, 1.0e-3f));

    const float temp = saturate(head * 1.35f + rail.g * 0.55f + streak * 0.5f
                              + chevron * 0.45f
                              + saturate(heat) * 0.35f + strike * 1.2f);
    const float3 warm = lerp(fringeColor.rgb, edgeColor.rgb, saturate(temp * 2.0f));
    const float3 tint = lerp(warm, coreColor.rgb, saturate(temp * 2.0f - 1.0f));
    // 筋は帯より必ず熱い。主色のままだと «太い金の線» になって刃に見えない。
    const float3 hot  = lerp(edgeColor.rgb, coreColor.rgb, 0.7f);

    // rgb = «足す光»。帯を先に成立させ、筋・繊維・火花はその上へ足すだけにする
    // (下の層を削る形にすると、細部を強くするほど軌跡が虫食いになる)。
    float3 emit = tint * body * max(bodyGain, 0.0f);
    // 節は «足す» 側にも回す。削るだけだと帯が暗くなるばかりで、流れているものが
    // 光って見えない。
    emit += lerp(edgeColor.rgb, coreColor.rgb, 0.4f) * chevron * max(bandBoost, 0.0f);
    emit += hot * rail * max(railBoost, 0.0f);
    // 内側は熱くしない。同じ明るさで 2 本引くと «どちらが刃か» が消える。
    emit += lerp(fringeColor.rgb, edgeColor.rgb, 0.55f) * spine * max(spineBoost, 0.0f);
    emit += coreColor.rgb * streak * max(streakGain, 0.0f);
    emit += coreColor.rgb * head * max(headBoost, 0.0f);
    emit += coreColor.rgb * sparks * max(sparkBoost, 0.0f);
    // 当たった «その位置» だけを一段押し上げる。芯の色で足すのは、命中を色相ではなく
    // 明度で言うため ─ 色を変えると «別のものが出た» に読まれる。
    emit += coreColor.rgb * strike * 2.2f;
    emit *= presence * rimGain * max(intensity, 0.0f)
          * (1.0f + saturate(heat) * 0.6f)
          * input.color.rgb * saturate(input.color.a);

    // 芯だけが背景を隠す。全体を隠すと、背景の明るい場所で帯が «灰色の板» になる。
    const float occlusion = saturate(rail.g * 0.85f + head * 0.5f + body * 0.30f
                                   + spine * 0.25f + strike * 0.6f)
                          * presence * saturate(opacity) * saturate(input.color.a);

    // 何も足さない画素まで帯として描くと、透明なのに深度とブレンドの帯域だけ食う。
    clip(max(occlusion, dot(emit, float3(1.0f, 1.0f, 1.0f))) - 0.0003f);
    return float4(emit, occlusion);
}
