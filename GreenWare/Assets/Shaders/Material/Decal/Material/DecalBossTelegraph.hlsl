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
///   pulse を毎フレーム進めることで入る (DecalPolarityRing と同じ形)。
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
    float2 _telegraphPad; // offset 88
};

/// 等間隔の帯 [0,1]。周期が閉じるので継ぎ目ができない。
///
/// WHY 三角波で作るか: sin だと縁が甘くなり、本数を増やしたとき «明るい面» へ潰れる。
///     三角波なら幅を直接指定できて角が立つ (DecalPolarityRing の目盛りと同じ理屈)。
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

    const float soft = max(saturate(edgeSoftness) * 0.15f, 1.0e-3f);
    const float half = max(frameWidth * 0.5f, 1.0e-3f);

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
    const float p      = saturate(progress);
    const float filled = 1.0f - smoothstep(p - soft, p + soft, dist);

    // 満ちの «先端» だけ強く光らせる。面が濃くなるだけだと «満ちている» のは分かっても
    // «今どこまで来たか» が読めない。動いている線が 1 本あると時間が絵になる。
    const float front = 1.0f - smoothstep(0.0f, max(frontWidth, 1.0e-3f), abs(dist - p));

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

    const float fillA  = fillColor.a * max(fillGain, 0.0f);
    float3 color = frameColor.rgb * frame * frameColor.a * frameGain
                 + fillColor.rgb  * filled * fillA * hole
                 + frameColor.rgb * pattern * fillA * 0.9f
                 + frameColor.rgb * front * inside * 1.6f
                 + frameColor.rgb * approach * 2.2f;
    float  alpha = saturate(frame * frameColor.a * frameGain
                          + filled * fillA * hole
                          + pattern * fillA * 0.9f
                          + front * inside * 0.8f
                          + approach * 0.9f);

    color *= max(pulse, 0.0f);
    alpha *= max(pulse, 0.0f);

    if (alpha <= 1.0e-3f)
        discard;

    return float4(color, saturate(alpha));
}
