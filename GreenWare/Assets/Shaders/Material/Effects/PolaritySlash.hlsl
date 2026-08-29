/// @file    PolaritySlash.hlsl
/// @brief   双剣の斬撃。刃の弧を «走り抜ける光 + 帯電した残り» の 2 段で描く
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// PSO: SOLID_NOCULL + PREMULTIPLIED + DEPTH_READ
///
/// 頂点は SlashArcComponent が MeshBuilder::AddRibbonFacing で組む帯で、
/// uv.x = 振り始めから振り抜きへの進み [0,1]、uv.y = 帯の横断 [0,1]。
/// 頂点カラーは層ごとの色味 (rgb) と重み (a)。
///
/// WHY 時計を 2 本持つか (progress / decay):
///   1 本の時計で «出て消える» を作ると、消し方をどれだけ工夫しても «点いて消えた» に
///   しかならない。斬撃で起きている出来事は 2 つあって、時間の桁が違う ─
///   刃が通り抜ける (0.1 秒) のと、通った跡の空気が帯電したまま崩れていく (0.4 秒)。
///   同じ時計に乗せると、速い方に合わせれば跡が残らず、遅い方に合わせれば刃が鈍る。
///   progress が刃、decay が跡。混ぜないから両方を «それらしい速さ» で出せる。
///
/// WHY 跡を «薄くする» のではなく «崩す» か:
///   一様に暗くする消し方は、明るさが下がるだけで形が最後まで残る。人の目は形の
///   変化を明るさより強く拾うので、消える瞬間に «絵が急に無くなった» と読める。
///   ノイズのしきい値で場所ごとに時間差をつけて千切ると、消えている最中も形が
///   動き続け、«まだ何かが起きている» ままフェードアウトできる。
///
/// WHY ギザギザを三角波で作るか:
///   sin も fbm も «丸い»。稲妻とほつれが稲妻に見えるのは折れ (角) があるからで、
///   滑らかな波をどれだけ重ねても角は生えない。abs(frac(x) - 0.5) は 1 命令で
///   折れを持つ三角波になる。周期の違う 2 枚を重ねると反復が読めなくなる。
///
/// WHY 雷を «蛇行 + 折れ + 途切れ» の 3 つで作るか:
///   蛇行だけだと光る紐、折れだけだと固いジグザグの模様、途切れだけだと点滅する線。
///   びりびりという感触の正体は «繋がっていないものが繋がって見える» ことなので、
///   3 つが同時に無いと «帯の上に描いた稲妻の絵» から抜けられない。
///   途切れはセルごとのハッシュで、位相を量子化した値を種に混ぜる。連続時間で
///   動かすと «滑らかに明滅する» になって、放電の不連続さが出ない。
///
/// WHY 企画書 12.2 のために白へ振り «切らない» か:
///   「発光を強くしすぎると白飛びして赤と青の区別がつかなくなる」。層が増えるほど
///   重なりで飽和しやすくなるので、白熱は «足す» のではなく最も明るい成分へ揃える
///   に留め、明るさの単位は intensity 1 本に持たせる (Beam.hlsl と同じ判断)。
///
/// WHY 事前乗算 (PREMULTIPLIED) か:
///   峰と雷は背景を隠して光り、外へ広がる暈と残光は背景へ光を足すだけ。ALPHA_BLEND
///   だと暈が背景を薄めて濁り、ADDITIVE だと峰まで透けて弧の芯が読めない。
///   out = src.rgb + dst.rgb * (1 - src.a) なら、rgb を «足す光»、a を «隠す量» として
///   独立に出せる。
///
/// WHY cbuffer の名前を変えてはいけないか:
///   ShaderDescriptor は PS バイトコードを GetConstantBufferByName("MaterialConstants")
///   で引いて変数表を作る。名前が違うと表が空のまま «有効な Descriptor» が返り、
///   MaterialInstance::Set* が per-instance 上書きを全部黙って捨てる。

#include "Common/Binding.hlsli"

#define FBZZ_MATERIAL_CONSTANTS
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    /// 弧の色と不透明度。rgb は極性色 × 明るさ (HDR)、a は «峰がどれだけ背景を隠すか»。
    /// SlashArcComponent が振った剣の極から毎フレーム書き込む。
    float4 albedo;

    // ── 時計 ────────────────────────────────────────────────────────────────
    /// 刃の進み [0,1]。0 で振り始め、1 で帯の末端まで抜け切る。速い方の時計。
    float  progress;
    /// 跡の崩れ [0,1]。0 で崩れ始め、1 で消え切る。遅い方の時計。
    float  decay;
    /// 乱れの位相。スクリプトが毎フレーム進める (整数周期で巻き取ること)。
    float  phase;
    /// 命中の «張り» [0,1]。当たった一撃だけ立てて減衰させる。
    float  flash;

    // ── 刃 ──────────────────────────────────────────────────────────────────
    /// 頭の鋭さ [uv.x 比]。小さいほど «刃» の切り口が硬くなる。
    float  headSharp;
    /// 頭の後ろに残す熱の長さ [uv.x 比]。掃過の尾そのものの長さ。
    float  tailLength;
    /// 全体の明るさ倍率 (HDR)。
    float  intensity;
    /// 峰を帯の横断のどこへ置くか [0,1]。0.5 で中央、1 に寄せるほど «刃の側» へ。
    float  edgeBias;

    /// 峰の幅 [半幅比]。細いほど鋭い線になる。
    float  edgeWidth;
    /// 峰の明るさ。暈との差が弧の «細さ» を決める。
    float  edgeBoost;
    /// 峰から離れるときの減衰指数。大きいほど暈が薄く狭くなる。
    float  bodyFalloff;
    /// 峰を白熱へ寄せる量 [0,1]。0 で純粋な極性色。
    float  edgeWhite;

    /// 白熱が届く長さ [uv.x 比]。頭からこの距離だけが白く、その後ろは極性色へ冷める。
    float  heatLength;
    /// 峰の色収差 [半幅比]。R と B を逆向きにずらして縁へ色を割る。
    float  dispersion;
    /// 振り始め側の立ち上がり [uv.x 比]。0 で切り口がそのまま出る。
    float  rootFade;
    /// 振り抜き側の減衰 [uv.x 比]。1 に近いほど先端が細く消える。
    float  tipFade;

    // ── ギザギザ ────────────────────────────────────────────────────────────
    /// 峰そのものを折る量 [半幅比]。0 で真っ直ぐな線。
    float  serrateAmp;
    /// 折れの細かさ [周/帯]。
    float  serrateScale;
    /// 崩れが進むほど折れを増やす量。跡が «千切れていく» のはこれが効く。
    float  serrateGrowth;
    /// 暈の輪郭を欠けさせる量 [0,1]。1 に近いほど外周がボロボロになる。
    float  serrateBite;

    // ── 雷 ──────────────────────────────────────────────────────────────────
    /// 帯を走る筋の本数 [0,4]。0 で出さない。
    float  boltCount;
    /// 筋の太さ [半幅比]。
    float  boltWidth;
    /// 蛇行の幅 [半幅比]。帯の中をどれだけ泳ぐか。
    float  boltWander;
    /// 蛇行へ混ぜる折れの量。0 だと «くねる紐» で稲妻にならない。
    float  boltJag;

    /// 進行方向の周期 [周/帯]。大きいほど細かく暴れる。
    float  boltTwist;
    /// 筋が流れる速さ [周/位相]。位相の巻き戻りで飛ばないよう 1/2^n を選ぶ。
    float  boltSpeed;
    /// 途切れの細かさ [区間/帯]。放電が «繋がっていない» 単位。
    float  boltBreak;
    /// 点いている区間の割合 [0,1]。1 で途切れない (ただの線に戻る)。
    float  boltGate;

    /// 筋の明るさ。
    float  boltBoost;
    /// 筋を白熱へ寄せる量 [0,1]。
    float  boltWhite;

    // ── びりびり (火花) ─────────────────────────────────────────────────────
    /// 縁で弾ける火花の密度 [個/帯]。0 で出さない。
    float  sparkDensity;
    /// 同時に点いている火花の割合 [0,1]。

    float  sparkRate;
    /// 火花の鋭さ。大きいほど点に近づく。
    float  sparkSharp;
    /// 火花の明るさ。
    float  sparkBoost;

    // ── 残光 ────────────────────────────────────────────────────────────────
    /// 刃が通った場所が保つ明るさの «床» [0,1]。掃過の熱が引いた後もここまでは残る。
    /// 0 にすると尾が消えて、弧が «頭だけの短い光» になる。
    float  residueGain;
    /// 崩れの細かさ [周/帯]。小さいと «大きな塊» で、大きいと «砂» で崩れる。

    float  dissolveScale;
    /// 崩れの縁の柔らかさ。0 に近いほど «割れる»、大きいほど «溶ける»。
    float  dissolveSoft;
    /// 崩れながら帯が横へ広がる量 [m]。VS が頂点を動かす。
    float  residueSpread;
    /// 崩れながら浮き上がる量 [m]。VS が頂点を動かす。
    float  residueLift;

    /// 刃が «今» 居る点の広がり [uv.x 比]。切っている位置そのものを示す白熱。
    float  headFlare;
    /// その明るさ。0 で出さない。
    float  headBoost;

    /// 崩れを外周へ寄せる指数。大きいほど峰の近くが崩れずに残る。
    /// 0 に近づけると帯全体が同時に崩れ、弧が途切れて見える。
    float  dissolveBias;
    /// 寿命のどこから全体を引き始めるか [0,1]。それまでは «在る» ままを保つ。
    float  residueFadeStart;

    /// 切り分け用の表示モード。0 = 通常。
    ///   1 … 帯を単色で塗り潰す。手続きの模様を一切通さない。
    ///        ここで縞や途切れが見えるなら、原因は模様ではなく
    ///        «帯の張り方 (幾何) / UV / ブレンド» のどれか。
    ///   2 … uv をそのまま色にする。R = 弧に沿った進み、G = 帯の横断。
    ///        R が端から端へ滑らかに変化しないなら UV が壊れている。
    ///        R が何度も 0→1 を繰り返すなら «繰り返しの模様» の正体はこれ。
    ///   3 … 在る量 (presence) だけを白で出す。時計と寿命の確認用。
    float  debugMode;
};

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"

struct SlashPSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
    /// 層ごとの色味と重み。SlashArcComponent が MeshBuilder::SetColor で層別に塗る。
    float4 color      : COLOR;
};

/// 折れを持つ三角波 [-0.5, 0.5]。稲妻とほつれの «角» はこれでしか出ない。
float SlashZig(float x)
{
    return abs(frac(x) - 0.5f) * 2.0f - 0.5f;
}

// ── 2 次元ノイズ ────────────────────────────────────────────────────────────
//
// WHY ArcNoise (1 次元) で代用してはいけないか:
//   ArcFbm(along * a + across * b) は «2 次元のノイズ» に見えるが、1 次元関数に
//   直線の式を食わせているだけで、値は a*x + b*y = 一定 の直線上でまったく動かない。
//   つまり模様が必ず斜めの縞になる。これをしきい値で切ると、帯が斜めに切り刻まれる。
//   ArcNoise.hlsli は «帯の長さ方向にだけ変化する乱れ» のための道具だと自分で
//   書いてあるとおりで、面で崩したいときは 2 次元の格子を引くしかない。
// WHY frac(sin(...)) を使わないか (縞に潰れていた原因):
//   ArcNoise.hlsli が自分で警告しているとおり、sin の引数が大きくなると float32 の
//   引数簡約で下位ビットが落ち、戻り値が «乱数» ではなく規則的な数列になる。
//   ここは 2 次元の格子番号 × 大きな係数を渡すので、その領域へすぐ入る
//   (格子 200 × 311 で sin(62000)、有効桁がほぼ残らない)。潰れたハッシュは
//   ノイズではなく縞を作り、しきい値で切ると帯が等間隔に途切れる。
//   sin を使わない構成なら、桁が伸びても値の分布が保たれる。
float SlashHash2(float2 cell)
{
    float3 p = frac(float3(cell.x, cell.y, cell.x) * float3(0.1031f, 0.1030f, 0.0973f));
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

/// 双一次補間の値ノイズ。
float SlashNoise2(float2 p)
{
    const float2 cell = floor(p);
    const float2 f    = frac(p);
    const float2 w    = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(SlashHash2(cell),                    SlashHash2(cell + float2(1.0f, 0.0f)), w.x),
                lerp(SlashHash2(cell + float2(0.0f, 1.0f)), SlashHash2(cell + float2(1.0f, 1.0f)), w.x),
                w.y);
}

/// 3 オクターブ。オフセットを足すのは、同じ格子が重なって縞が戻るのを防ぐため。
float SlashFbm2(float2 p)
{
    return SlashNoise2(p) * 0.55f
         + SlashNoise2(p * 2.17f + 13.7f) * 0.30f
         + SlashNoise2(p * 4.63f + 41.3f) * 0.15f;
}

/// 1 次元の値ノイズ。2 次元格子を lane で切って引く。
/// WHY ArcNoise を使わないか: 同じ frac(sin()) の桁落ちに当たる。筋ごとに lane を
///     ずらせば、同じ格子から独立した列がいくらでも取れる。
float SlashNoise1(float x, float lane)
{
    const float i = floor(x);
    const float f = frac(x);
    const float w = f * f * (3.0f - 2.0f * f);
    return lerp(SlashHash2(float2(i, lane)), SlashHash2(float2(i + 1.0f, lane)), w);
}

float SlashFbm1(float x, float lane)
{
    return SlashNoise1(x, lane) * 0.55f
         + SlashNoise1(x * 2.3f, lane + 17.0f) * 0.30f
         + SlashNoise1(x * 5.1f, lane + 41.0f) * 0.15f;
}

/// 周期の違う 2 枚を重ねた折れ線。1 枚だと反復が読める。
float SlashJag(float x)
{
    return SlashZig(x) * 0.65f + SlashZig(x * 2.31f + 0.37f) * 0.35f;
}

/// 峰を 1 本のガウスで作る。色収差は横断位置をずらして 3 回呼ぶ。
float SlashEdge(float lateral)
{
    const float offsetFromEdge = lateral / max(edgeWidth, 1.0e-3f);
    return exp(-offsetFromEdge * offsetFromEdge);
}

SlashPSIn VSMain(VSInputColor v)
{
    // WHY 頂点を動かすか (崩れの «動き»):
    //   色だけで消していくと、跡は «同じ形のまま暗くなる» ので、消えている最中に
    //   何も起きていない時間ができる。散った熱は広がって浮くので、帯を横へ開いて
    //   持ち上げれば、消え際まで形が動き続けて «まだ何かが起きている» ままになる。
    //
    // WHY 帯の横方向を法線と接線から作るか: AddRibbonFacing は normal = cross(tangent, side)
    //   で頂点を積む。逆に解けば side が戻るので、CPU から余分な情報を渡さずに
    //   «帯の幅の向き» が取れる (帯はカメラへ正対しているので、これは常に画面内の横)。
    const float3 side  = normalize(cross(v.normal, v.tangent));
    const float  rot   = saturate(decay);
    const float  outer = (v.uv.y - 0.5f) * 2.0f;

    float3 local = v.position;
    local += side * outer * residueSpread * rot;
    // 浮きは二乗で効かせる。線形だと «最初から上へ動いている» ように見えて、
    // 刃が通った直後の «その場に在る» が出ない。
    local.y += residueLift * rot * rot;

    SlashPSIn o;
    const float4 worldPos = mul(float4(local, 1.0f), world);
    o.svPosition = mul(worldPos, viewProjection);
    o.uv    = v.uv;
    o.color = v.color;
    return o;
}

float4 PSMain(SlashPSIn input) : SV_Target0
{
    const float along  = saturate(input.uv.x);
    const float across = saturate(input.uv.y);
    const float rot    = saturate(decay);

    // ── 切り分け ────────────────────────────────────────────────────────────
    // 手続きの模様を通さずに «帯そのもの» を見る。原因がこのファイルの中なのか、
    // 幾何 / UV / 描画状態なのかは、絵を見比べる以外に切り分けようがない。
    if (debugMode > 0.5f) {
        // 1: 単色。帯の形とブレンドだけが出る。
        if (debugMode < 1.5f) return float4(albedo.rgb * 0.6f, 1.0f);
        // 2: uv をそのまま。R が 0→1 を一度だけ登れば UV は正しい。
        if (debugMode < 2.5f) return float4(along, across, 0.0f, 1.0f);
    }

    // ── 時計 ────────────────────────────────────────────────────────────────
    // 頭は «uv.x のどこまで刃が通ったか»。尾のぶんだけ 1 を超えて進ませることで、
    // progress = 1 のときに帯の末端まで熱が抜け切る。
    const float tail   = max(tailLength, 1.0e-3f);
    const float head   = saturate(progress) * (1.0f + tail);
    // 正で «頭が通り過ぎた»、負で «まだ来ていない»。
    const float behind = head - along;

    // 頭の前は何も出さない。刃の位置がそのまま切り口になる。
    const float lead = smoothstep(0.0f, max(headSharp, 1.0e-4f), behind);

    // ── ギザギザ ────────────────────────────────────────────────────────────
    // 峰そのものを折る。折れは時間が経つほど深くなる ─ 通った直後は鋭い切り口で、
    // 崩れるにつれて «裂け目» へ変わっていく。
    const float jagAmp   = serrateAmp * (1.0f + serrateGrowth * rot);
    const float jagScale = max(serrateScale, 0.0f);
    // WHY 三角波だけにしないか: 三角波は完全な周期関数なので、折れが等間隔に並ぶ。
    //     «ギザギザ» ではなく «のこぎりの歯» に見えてしまい、模様として読まれる。
    //     角は三角波が、不揃いはノイズが受け持つ。
    const float jagLine = SlashJag(along * jagScale + phase * 0.5f) * 2.0f * 0.65f
                        + (SlashFbm2(float2(along * jagScale * 0.7f, phase * 0.6f)) - 0.5f)
                          * 2.0f * 0.35f;
    const float bias    = saturate(edgeBias) + jagLine * jagAmp;

    // 峰からの符号付き距離。外側 (刃の側) と内側 (尾の側) で残りの幅が違うので、
    // それぞれの側の幅で割って [-1,1] に正規化する。こうしないと bias が端へ寄った
    // 瞬間に、狭い側だけ峰が帯からはみ出して四角く切れる。
    const float safeBias = clamp(bias, 0.05f, 0.95f);
    const float offset   = across - safeBias;
    const float span     = offset > 0.0f ? (1.0f - safeBias) : safeBias;
    const float lateral  = offset / max(span, 1.0e-3f);

    // ── 崩れ ────────────────────────────────────────────────────────────────
    // 場所ごとに «消える時刻» をずらす。一様に暗くすると形が最後まで残り、
    // 消える瞬間に絵が急に無くなったと読める。
    // 帯の長さ方向を細かく、横断方向を粗く引く。横断まで細かくすると帯が «砂» に
    // なって、刃の軌跡ではなく煙に見える。
    const float dissolveNoise = SlashFbm2(float2(along * max(dissolveScale, 0.0f)
                                                     + phase * 0.25f,
                                                 across * 1.7f));
    const float soft = max(dissolveSoft, 1.0e-3f);

    // ── そこに光が «在る» 量 ────────────────────────────────────────────────
    //
    // WHY 尾を 0 へ落とさないか (弧が途切れて見えた構造上の原因):
    //   掃過の熱だけで «在る» を作ると、頭から tailLength より後ろは残光しか支えが
    //   無くなる。その残光を崩れのしきい値で穴だらけにしていたので、弧が
    //   «明るい頭 + まばらな斑点» に割れて、1 本の斬撃として読めなくなっていた。
    //   刃が通った場所は必ず床の明るさを保ち、消すのは寿命の終わりに全体を一度だけ。
    //
    // WHY 最後の引きを smoothstep で締めるか: 崩れのしきい値だけだと、ノイズが 1 に
    //   近い画素は decay = 1 でも消え残る。跡の寿命が尽きた瞬間に枠が畳まれるので、
    //   そこだけプツッと切れる。どの画素も 0 へ着地することを保証する。
    const float hotTail  = 1.0f - smoothstep(0.0f, tail, max(behind, 0.0f));
    const float glow     = max(hotTail, saturate(residueGain));
    const float settle   = 1.0f - smoothstep(saturate(residueFadeStart), 1.0f, rot);
    const float presence = lead * glow * settle;

    // ── 崩れ ────────────────────────────────────────────────────────────────
    //
    // WHY 芯を崩さないか: 弧が «1 本の線» として読めるのは峰が繋がっているからで、
    //   そこへ穴を開けると、どれだけ細部を足しても «途切れた光» にしかならない。
    //   崩れは外周ほど強く効かせ、峰は最後まで繋げたままにする。
    //
    // WHY 深さを 1 つの係数で括るか: 途切れて見えたときに «どれを下げれば繋がるか» が
    //   1 つに定まっていないと切り分けられない。serrateBite = 0 で崩れが完全に消え、
    //   弧は必ず 1 本の帯になる。
    const float rimness   = saturate(abs(lateral));
    const float cut       = rot * pow(rimness, max(dissolveBias, 0.01f));
    const float tatterRaw = 1.0f - smoothstep(dissolveNoise - soft,
                                              dissolveNoise + soft, cut);
    const float tatter    = lerp(1.0f, tatterRaw, saturate(serrateBite));

    // 3: 在る量だけ。時計 (progress / decay) と寿命の確認用。
    if (debugMode > 2.5f) return float4(presence.xxx, presence);

    // ── 刃 ──────────────────────────────────────────────────────────────────
    // R と B を逆向きにずらして縁へ色を割る。中央 (G) は必ず本来の位置で採るので、
    // 色収差を上げても峰の «位置» そのものは動かない。
    const float3 edge = float3(SlashEdge(lateral + dispersion),
                               SlashEdge(lateral),
                               SlashEdge(lateral - dispersion));

    // 峰の外へ広がる暈。内側 (尾の側) を緩く落として «刃の後ろに残っている» を作る。
    // 対称に落とすと、どちらが刃でどちらが尾なのかが 1 枚の帯から読めない。
    const float bodyLateral = offset < 0.0f ? lateral * 0.55f : lateral;
    // WHY ここで輪郭を欠けさせないか: 以前は暈にも三角波の «噛み跡» を掛けていたが、
    //     暈は弧の «太さ» を伝える層で、そこが等間隔に凹むと帯が節に割れて見える。
    //     欠けは崩れ (tatter) が外周だけに与えるので、この層は滑らかなまま残す。
    const float body = pow(saturate(1.0f - abs(bodyLateral)), max(bodyFalloff, 0.01f));

    // ── 雷 ──────────────────────────────────────────────────────────────────
    // 蛇行 (泳ぐ) + 折れ (角) + 途切れ (不連続) の 3 つが揃って初めて放電に見える。
    const float strands   = clamp(boltCount, 0.0f, 4.0f);
    const float boltHalf  = max(boltWidth, 1.0e-3f);
    // 位相を量子化した値を種に混ぜる。連続時間で動かすと «滑らかに明滅する» になり、
    // 放電の «点いたり消えたり» が出ない。
    //
    // WHY 刻みを粗くするか: 1 フレームに 2 回以上引き直すと、点いている区間が
    //     1 フレームも保たずに入れ替わる。放電ではなく «画面全体のちらつき» として
    //     見えて、しかも動画にすると帯が虫食いに映る。2〜3 フレーム保つ速さにする
    //     (crackleRate 9 なら 1 秒に 22 回 = 60fps で約 2.7 フレームに 1 回)。
    const float strobe    = floor(phase * 2.5f);
    float bolts = 0.0f;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        // WHY break ではなく重みで消すか: 本数は per-instance で毎フレーム変わりうる。
        //     分岐で抜くと «本数だけが違う 4 通り» が動的分岐として残り、隣接画素で
        //     経路が割れる。掛け算なら常に同じ命令列で済む。
        const float active = step((float)i + 0.5f, strands);
        const float lane   = along * max(boltTwist, 0.0f)
                           + phase * boltSpeed
                           + (float)i * 1.618f;
        // 泳ぎは滑らかなノイズ、角は三角波。片方だけでは紐かジグザグの模様になる。
        const float wander = ((SlashFbm1(lane, (float)i * 9.0f) - 0.5f) * 2.0f) * boltWander
                           + SlashJag(lane * 3.1f) * 2.0f * boltJag;
        const float bolt   = 1.0f - smoothstep(0.0f, boltHalf, abs(lateral - wander));
        // 区間ごとに点いているかどうかを引き直す。ここが «びりびり» の正体。
        //
        // WHY 2 次元で引くか: 区間番号と時刻を 1 本の数へ畳むと、係数を掛けた時点で
        //     桁が伸びてハッシュが潰れる。格子の 2 軸に分けて渡せば、どちらも
        //     小さい整数のまま済む。
        const float cell = floor(along * max(boltBreak, 1.0f) + (float)i * 7.3f);
        const float gate = step(1.0f - saturate(boltGate),
                                SlashHash2(float2(cell + (float)i * 3.1f, strobe)));
        bolts += bolt * gate * active;
    }
    // 筋が交差した点は明るくてよいが、本数だけ足し込むと本数がそのまま白飛びの
    // 回数になる。交差が読める程度で頭を打たせる。
    bolts = min(bolts, 1.8f);

    // ── びりびり (火花) ─────────────────────────────────────────────────────
    // 帯の «外» で弾ける短い光。刃の線から離れたところに散らすことで、弧そのものが
    // 帯電しているように見える。
    float sparks = 0.0f;
    if (sparkDensity > 0.0f) {
        const float cell = floor(along * sparkDensity);
        const float seed = SlashHash2(float2(cell, strobe));
        const float on   = step(1.0f - saturate(sparkRate), seed);
        // 区間の中の «どこで» 光るかも引き直す。中央固定だと等間隔の点線になる。
        const float at   = SlashHash2(float2(cell + 37.0f, strobe + 11.0f));
        const float dot0 = 1.0f - abs(frac(along * sparkDensity) - at) * 2.0f;
        // 縁の側へ寄せる。中央で光らせると峰に埋もれて «太い線» にしかならない。
        const float rim  = pow(saturate(abs(lateral)), 2.0f);
        sparks = pow(saturate(dot0), max(sparkSharp, 1.0f)) * on * rim;
    }

    // ── 両端 ────────────────────────────────────────────────────────────────
    // 切り口を隠す。絞らないと帯の端が四角いまま残り、光る板が生えて見える。
    const float root = smoothstep(0.0f, max(rootFade, 1.0e-4f), along);
    const float tip  = 1.0f - smoothstep(1.0f - max(tipFade, 1.0e-4f), 1.0f, along);
    const float ends = root * tip;

    // ── 色 ──────────────────────────────────────────────────────────────────
    // 斬り抜けた点だけが白熱し、後ろは極性色へ冷める。命中したときだけ flash が
    // 一段押し上げる。白は «足す» のではなく最も明るい成分へ揃えるので飽和しない。
    const float  heat = exp(-max(behind, 0.0f) / max(heatLength, 1.0e-3f));
    const float3 rail = albedo.rgb * input.color.rgb;
    const float  peak = max(max(rail.r, rail.g), rail.b);
    const float3 white = float3(peak, peak, peak);
    const float3 hot  = lerp(rail, white, saturate(edgeWhite * heat + saturate(flash) * 0.5f));
    // 雷と火花は掃過が過ぎた後も走り続けるので、熱ではなく固定量で白へ寄せる。
    const float3 arcHot = lerp(rail, white, saturate(boltWhite));

    const float weight = max(intensity, 0.0f) * (1.0f + saturate(flash))
                       * ends * saturate(input.color.a);

    // 刃が «今» 居る点。前後へ短く広がる白熱で、切っている位置そのものを示す。
    //
    // WHY 前 (まだ通っていない側) にも出すか: 掃過の頭は lead で硬く切ってあるので、
    //     そこだけ見ると «光が生えてくる» 見え方になる。通る直前の空気が張り詰める
    //     ぶんだけ先へ滲ませると、刃が «来る» という予兆が 1〜2 フレーム先に立つ。
    //
    // WHY presence を掛けないか: presence は頭より後ろにしか無い。掛けると前側の
    //     滲みが丸ごと消えて、この項の意味が無くなる。
    const float headBand  = exp(-abs(behind) / max(headFlare, 1.0e-3f));
    const float headShape = headBand * (behind < 0.0f ? 0.35f : 1.0f)
                          * pow(saturate(1.0f - abs(lateral)), 1.5f);

    // rgb = «足す光»。
    //
    // WHY 雷と火花を «足すだけ» にするか: 電気の細部が下の層を削る形にすると、
    //     細部を強くするほど弧が虫食いになる。芯と暈で 1 本の帯を先に成立させ、
    //     放電はその上へ重ねるだけにすれば、どれだけ足しても途切れない。
    //
    // WHY 峰に tatter を掛けないか: 崩れてよいのは «広がり» であって «線» ではない。
    float3 emit = rail * body * presence * tatter;
    emit += hot * edge * max(edgeBoost, 0.0f) * presence;
    emit += arcHot * bolts * max(boltBoost, 0.0f) * presence;
    emit += arcHot * sparks * max(sparkBoost, 0.0f) * presence;
    emit += hot * headShape * max(headBoost, 0.0f);
    emit *= weight;

    // 峰と雷だけが背景を隠し、暈と火花は加算グローとして素通しにする。
    // WHY 暈を少しだけ混ぜるか: 峰だけを隠す量にすると、明るい背景の上で弧の外周が
    //     完全に消え、細い線 1 本だけが残って «斬った» 幅が読めなくなる。
    const float occlusion = saturate(edge.g * 0.9f * presence
                                   + bolts * 0.5f * presence
                                   + headShape * 0.6f
                                   + body * 0.35f * presence * tatter)
                          * albedo.a * weight;

    // 何も足さない画素まで帯として描くと、透明なのに深度とブレンドの帯域だけ食う。
    // WHY しきい値を下げたか: 薄い暈は 1/1000 の桁で効いている。0.002 で切ると、
    //     弧の外周が «まだら» に抜けて、そこが途切れとして見えていた。
    clip(max(occlusion, dot(emit, float3(1.0f, 1.0f, 1.0f))) - 0.0003f);
    return float4(emit, occlusion);
}
