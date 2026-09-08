/// @file DecalBossTelegraph.hlsl
/// @brief ボスの攻撃が来る範囲を地面へ描く予兆。円 (着弾点) と帯 (突進・ビーム) の 2 形
/// @author Hasegawa Jin
/// @date 2026-08-30
///
/// WHY 予兆が要るか:
///   ボスは全高 6m で、腹下に潜って斬るのが基本の間合いになる。プレイヤーの視界は
///   ほぼ脚と胴の裏側で、上で何が振り上がっているかは画面に映らない。避ける判断を
///   «見て» させるには、危ない範囲を足元へ落とすしかない。
///
/// WHY 円と帯を 1 枚のシェーダーで持つか:
///   4 種類の攻撃に 4 枚の .mat を作ると、濃さ・縁の甘さ・色の «揃え直し» が
///   毎回発生する。読み方は «自分がこの形の中に居るか» の 1 つしかないので、
///   形の違いは shape の分岐 1 つで足りる。
///
/// WHY 塗りではなく «枠 + 満ちていく帯» か:
///   面を塗ると、そこに立っているプレイヤー自身が塗り潰されて足元が見えなくなる。
///   避けるのに要るのは «縁がどこか» と «あと何秒か» の 2 つだけなので、
///   枠を出して内側を progress ぶんだけ満たす。中央は抜いておく。
///
/// WHY 時刻を持たないか:
///   デカールの cbuffer に時刻が無い (DecalCommon.hlsli)。明滅は呼び出し側が
///   pulse を毎フレーム進めることで入る (手続きデカールと同じ形)。
///
/// 既定値の取り方: InitDefaultMaterialParams が全 float を 1.0 で敷くため、
///     1.0 が «安全側» になるよう意味を選んである (progress 1 = 満ちきり、
///     edgeSoftness 1 = 甘い縁)。
#include "Material/Decal/DecalCommon.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 frameColor;    // offset  0  枠の色 (a = 不透明度)
    float4 fillColor;     // offset 16  満ちる帯の色 (a = 不透明度)

    float  shape;         // offset 32  0 = 円 / 1 = 帯
    float  progress;      // offset 36  予兆の進み [0,1]。1 で着弾
    float  frameWidth;    // offset 40  枠の太さ (正規化距離)
    float  edgeSoftness;  // offset 44  縁のぼけ [0,1]。内部で 0.15 倍して使う

    float  innerFade;     // offset 48  中心を抜く量。1 で中心が完全に透ける
    float  pulse;         // offset 52  全体の明るさ倍率。呼び出し側が明滅させる
    float  fillGain;      // offset 56  満ちる帯の濃さ
    float  frontWidth;    // offset 60  満ちの先端を光らせる幅 (正規化距離)

    float  stripeCount;   // offset 64  斜線の本数。0 で斜線なし
    float  stripeWidth;   // offset 68  斜線 1 本の幅 [0,1]
    float  stripeScroll;  // offset 72  斜線の流れる位相 [周]。呼び出し側が進める
    float  chevronCount;  // offset 76  帯に出す矢羽根の数。0 で矢羽根なし

    // 危険が «上から» 来るか。0 = 地を這う (既定) / 1 = 落ちてくる。
    //
    // WHY 形ではなくこれで分けるか: 踏みつけと磁力パルスはどちらも円で、
    //     床に出る絵が同じになる。しかし避け方は «真下から出る» と «外へ逃げる» で
    //     正反対。色を増やす手は使えない (赤青は極性・琥珀は危険で埋まっている) ので、
    //     «動く向き» を情報にする。
    float  threatAbove;   // offset 80
    float  approachWidth; // offset 84  落下リングの太さ (正規化距離)

    // 帯が «進む» 手か。0 = その場で太る (叩きつけ・檻) / 1 = 始点から終点へ走る。
    //
    // WHY 要るか: 帯の満ちは中心線から横へ広がる作りだった。だが薙ぎ・噛みつき・
    //     走りは «帯に沿って» 来る手で、太っていく絵は起きることと違う。しかも
    //     矢羽根の流れは経過時間の一定速度で progress と無関係だったので、
    //     帯の予兆は «あと何秒» をほとんど伝えていなかった。
    //     叩きつけは一斉に落ちるので、そちらは横へ太るのが正しい。
    float  travel;        // offset 88

    // 回避窓の入口 [0,1]。この進みから «今» の状態へ入り、枠が白へ寄って太る。
    //
    // WHY 明るさのランプでは足りないか: 予兆の進みは frameGain (0.75→1.45) と
    //     満ちる面で既に連続量として出ている。人は連続量から着弾時刻を当てるのが
    //     苦手で、要るのは «質の変化» ─ 色と太さが切り替われば «今» が読める。
    //     1.0 で窓なし (既定値が安全側)。
    float  strikeWindow;  // offset 92

    // 枠に刻む拍の数。明るい弧がこの目盛りを 1 つ越えるたびに 1 拍。
    //
    // WHY 目盛りが要るか: 弧が伸びるだけだと «あと何割» しか分からない。
    //     等間隔の刻みがあると «あと 2 つ» と数えられ、リズムで避けられる。
    float  countPips;     // offset 96

    // 着弾の «弾け» の残り [0,1]。1 = 通常表示 / 0 = 弾け切り。
    //
    // WHY 残量で持つか: 未設定の float は 1.0 で敷かれる (InitDefaultMaterialParams)。
    //     «弾けた量» で持つと、書き忘れた材質が最初から消えている状態になる。
    float  burstFade;     // offset 100

    float2 _telegraphPad; // offset 104
};

/// 等間隔の帯 [0,1]。周期が閉じるので継ぎ目ができない。
///
/// WHY 三角波で作るか: sin だと縁が甘くなり、本数を増やしたとき «明るい面» へ潰れる。
///     三角波なら幅を直接指定できて角が立つ (手続きデカールの目盛りと同じ理屈)。
float StripeBand(float coord, float count, float width, float aa)
{
    if (count < 0.5f)
        return 0.0f;

    const float phase = abs(frac(coord * count) - 0.5f) * 2.0f;
    const float half  = saturate(width);
    return 1.0f - smoothstep(saturate(half - aa), saturate(half + aa), 1.0f - phase);
}

DecalPixelInput VSMain(uint id : SV_VertexID)
{
    return DecalVertexMain(id);
}

float4 PSMain(DecalPixelInput input) : SV_Target
{
    DecalSurface surface;
    if (!DecalResolve(input.screenUv, surface))
        discard;

    const float2 offset = surface.uv - 0.5f;

    // «縁までの正規化距離» を形ごとに 1 本の値へ畳む。以降の枠・満ち・抜きは
    // どちらの形でも同じ式で書ける。
    //
    // 円 … 中心からの距離。1.0 が OBB の外接円 = 指定した半径
    // 帯 … 横方向だけの距離。長さ方向は端のフェードだけが見る
    float dist;
    float along;   // 帯の «進行方向» の位置 [0,1]。円では中心からの距離を流用する
    if (shape < 0.5f) {
        dist  = length(offset) * 2.0f;
        along = dist;
    } else {
        dist  = abs(offset.x) * 2.0f;
        along = saturate(surface.uv.y);
    }

    // 満ちに使う座標。«進む帯» だけ進行方向へ切り替える。
    //
    // WHY 枠と分けるか: 枠は «縁がどこか» を言う空間の情報で、満ちは «あと何秒» を
    //     言う時間の情報。帯が進む手では 2 つの向きが直交するので、同じ dist に
    //     兼ねさせると «太る» か «走る» のどちらかしか出せない。
    const bool  runs  = (shape >= 0.5f) && (travel >= 0.5f);
    const float sweep = runs ? along : dist;

    // 枠を «時計» として読ませるための座標 [0,1)。
    // 円は角度 (上から時計回り)、帯は進行方向。明るい弧がここを 0 → progress まで
    // 伸び、一周して閉じた瞬間が着弾になる。
    //
    // WHY 半径方向の満ちと別の軸を使うか: 満ちは «どこが危ないか» を広げる絵で、
    //     着弾の瞬間に縁 (枠) と重なって消える。時間そのものを別の軸へ逃がせば、
    //     一番大事な «今» が他の要素と融合しない。
    float clock;
    if (shape < 0.5f) {
        clock = frac(atan2(offset.y, offset.x) * 0.15915494f + 0.75f);
    } else {
        clock = along;
    }

    const float soft = max(saturate(edgeSoftness) * 0.15f, 1.0e-3f);

    // 回避窓。ここへ入ると枠が白へ寄って太る ─ 連続ランプではなく状態の切り替え。
    const float p0     = saturate(progress);
    const float strike = saturate((p0 - saturate(strikeWindow))
                                / max(1.0f - saturate(strikeWindow), 1.0e-3f));

    // 弾けは «外へ» ではなく «枠が太って薄れる» で出す。デカールの箱は指定半径ぴったり
    // なので、外側へ広げた輪は切り取られて出ない。
    const float fade  = saturate(burstFade);
    const float burst = 1.0f - fade;
    const float half  = max(frameWidth * 0.5f, 1.0e-3f)
                      * (1.0f + strike * 0.9f + burst * 2.2f);

    // ── 枠 ────────────────────────────────────────────────────────────────
    // 外周のすぐ内側へ 1 本。«ここから先が危ない» の線そのもの。
    const float toFrame = abs(dist - 1.0f);
    float frame = 1.0f - smoothstep(half, half + soft, toFrame);

    // 帯は «長さの両端» にも枠が要る。無いと画面外まで続いているように見える。
    if (shape >= 0.5f) {
        const float toEnd = min(along, 1.0f - along) * 2.0f;
        frame = max(frame, 1.0f - smoothstep(half, half + soft, toEnd));
        // 帯の外側は描かない。OBB の角が丸く残ると «円のつもり» に読める。
        frame *= 1.0f - smoothstep(1.0f, 1.0f + soft, dist);
    }

    // ── 満ちる面 ──────────────────────────────────────────────────────────
    // WHY 内側から外へ満たすか: 外から内へ縮めると «安全になっていく» に見える。
    //     危険は増えていくので、満ちる向きは «広がる» でなければ意味が反転する。
    const float p      = p0;
    const float filled = 1.0f - smoothstep(p - soft, p + soft, sweep);

    // 満ちの «先端» だけ強く光らせる。面が濃くなるだけだと «満ちている» のは分かっても
    // «今どこまで来たか» が読めない。動いている線が 1 本あると時間が絵になる。
    const float front = 1.0f - smoothstep(0.0f, max(frontWidth, 1.0e-3f), abs(sweep - p));

    // ── 時計の弧と目盛り ──────────────────────────────────────────────────
    // 枠の上を «明るい弧» が 0 → progress まで伸びる。一周して閉じた瞬間が着弾。
    //
    // WHY 収束マーカーを外から持ってこないか: デカールの箱は指定半径ぴったりなので、
    //     枠の外を通る輪は描けない。同じ «判定線へ到達する» 仕掛けを、半径ではなく
    //     枠に沿った 1 次元の上で作れば、円でも帯でも同じ 1 つの読み方になる。
    const float clockAA = max(soft * 2.0f, 4.0e-3f);
    const float swept   = 1.0f - smoothstep(p, p + clockAA, clock);

    // 目盛り。拍の位置に暗い切れ目を入れて «あと 2 つ» と数えられるようにする。
    float pipGap = 0.0f;
    if (countPips >= 1.5f) {
        const float t = frac(clock * countPips);
        pipGap = 1.0f - smoothstep(0.0f, clockAA * 1.6f, min(t, 1.0f - t));

        // 帯の «両端の蓋» には刻まない。clock (= along) は端でちょうど 0 と 1 に
        // なるので、そのまま掛けると 1 拍目と最後の拍の切れ目が蓋を丸ごと消し、
        // 帯が «画面外まで続いている» ように見える。
        if (shape >= 0.5f)
            pipGap *= smoothstep(0.0f, 0.06f, min(along, 1.0f - along));
    }

    // ── 斜線ハッチ ────────────────────────────────────────────────────────
    // WHY 斜線か: «危険» を示す語彙として一番読み違えられない。塗り潰しは «床の模様» と
    //     区別が付かず、点滅は «いつ避けるか» を潰す。流れる向きがあると、
    //     帯では «どちらから来るか» まで同じ模様で言える。
    const float hatchCoord = (shape < 0.5f)
        ? (surface.uv.x + surface.uv.y) - stripeScroll
        : along - stripeScroll;
    const float hatch = StripeBand(hatchCoord, stripeCount, stripeWidth, soft * 2.0f);

    // ── 矢羽根 (帯だけ) ───────────────────────────────────────────────────
    // WHY 帯にだけ出すか: 円は «そこへ来る» で向きを持たない。帯は始点と終点が
    //     あるので、向きが出ていないと «どちらから来るのか» が形から読めない。
    float chevron = 0.0f;
    if (shape >= 0.5f) {
        // 横へ行くほど手前へずらすと V 字になる。
        const float chevCoord = along - dist * 0.25f - stripeScroll;
        chevron = StripeBand(chevCoord, chevronCount, stripeWidth * 0.8f, soft * 2.0f);
    }

    // 中心は抜く。足元が塗りで潰れると、避ける先も自分の位置も読めなくなる。
    const float hole = smoothstep(0.0f, max(saturate(innerFade), 1.0e-3f), dist);

    // 危険域の «中» にだけ模様を出す。外へはみ出すと、まだ安全な床まで危なく見える。
    const float inside = (1.0f - smoothstep(1.0f - soft, 1.0f, dist)) * hole;
    const float pattern = saturate(max(hatch, chevron)) * inside * filled;

    // ── 落下リング (円 × «上から» のときだけ) ────────────────────────────────
    // 外周から中心へ縮み、progress = 1 で中心へ到達する。満ちる面 (外向き) と
    // 逆向きに動くので、同じ円のまま «降ってくる» と «広がる» が区別できる。
    //
    // WHY 帯には出さないか: 帯は矢羽根が既に «どちらから来るか» を言っている。
    //     そこへ縮むリングを重ねると、進行方向が 2 つあるように見える。
    float approach = 0.0f;
    if (threatAbove >= 0.5f && shape < 0.5f) {
        const float ringR = 1.0f - p;
        approach = 1.0f - smoothstep(0.0f, max(approachWidth, 1.0e-3f), abs(dist - ringR));
        // 中心へ着く瞬間がいちばん明るい。着弾の «点» が絵になる。
        approach *= lerp(0.55f, 1.0f, p);
    }

    // 枠は着弾が近いほど強く。予兆が出た瞬間と直前が同じ濃さだと、進みが枠から読めない。
    const float frameGain = lerp(0.75f, 1.45f, p);

    // 枠を «まだ» と «もう» の 2 層に割る。暗い方は範囲の表示、明るい弧が時間の表示。
    // 目盛りの切れ目は両方から抜く ─ 抜けていないと «何拍目» が数えられない。
    const float notch    = 1.0f - pipGap;
    const float frameDim = frame * notch * 0.34f;
    const float arc      = frame * notch * swept;

    // 回避窓では枠が «白へ» 寄る。琥珀のまま明るくするだけでは、離れて見たときに
    // 進みの続きにしか見えない (赤青は極性で埋まっているので色相は動かせない)。
    const float3 strikeColor = lerp(frameColor.rgb, float3(1.0f, 0.95f, 0.86f),
                                    saturate(strike * 0.85f + burst));
    const float  strikeGain  = 1.0f + strike * 1.1f + burst * 2.4f;

    const float fillA  = fillColor.a * max(fillGain, 0.0f);
    float3 color = frameColor.rgb * frameDim * frameColor.a
                 + strikeColor    * arc * frameColor.a * frameGain * strikeGain
                 + fillColor.rgb  * filled * fillA * hole
                 + frameColor.rgb * pattern * fillA * 0.9f
                 + strikeColor    * front * inside * 1.6f
                 + frameColor.rgb * approach * 2.2f;
    float  alpha = saturate(frameDim * frameColor.a
                          + arc * frameColor.a * frameGain
                          + filled * fillA * hole
                          + pattern * fillA * 0.9f
                          + front * inside * 0.8f
                          + approach * 0.9f);

    // 弾けは «薄れて消える»。着弾の後も 1 コマ残ると «今のが着弾だった» が読める。
    color *= max(pulse, 0.0f);
    alpha *= max(pulse, 0.0f) * fade;

    if (alpha <= 1.0e-3f)
        discard;

    return float4(color, saturate(alpha));
}
