/// @file    Outline.hlsl
/// @brief   極を帯びた対象のシルエットへ、放電しているアウトラインを掛ける
/// @author  Hasegawa Jin
/// @date    2026-08-28
///
/// customParameters
///   x — 輪郭の最大の太さ [px]。マスクの A (要求ごとの太さ 0..1) がこれを縮める
///   y — ビリビリの強さ 0..1。0 で滑らかな等幅の輪郭
///   z — 明滅の速さ [Hz]
///   w — 明るさ。LDR で足すので 1 を超えた分は頭打ちになる
/// customParameters2 (放電)
///   x — 抽選を引き直す速さ [Hz]。速すぎると «放電» ではなく «画面のちらつき» になる
///   y — 同時に放電する方向の割合 0..1。1 に近いと全周が伸びて太い輪郭へ戻る
///   z — 放電した方向が伸びる倍率。1 で伸びない
///   w — 筋の細さ。低いと帯が明るくなるだけで «線» に見えない
/// customIntensity — 全体の強さ 0..1
///
/// stage = PostProcess で走らせる前提。
/// 入力の t5 は前段の画、t6 は ObjectMaskPass が描いたシルエット
/// (RGB = 極の色 / A = 太さ)。
///
/// WHY 色をここで決めないか:
///   ＋と − が同時に盤面へ並ぶ。パスの定数で 1 色に決めると、どちらの極が
///   帯電しているのかが輪郭から読めず、色に意味を持たせた 12.2 が崩れる。
///   誰が何色かはマスクが 1 フェッチで答える。
///
/// WHY マスクを point で引くか:
///   マスクの A は «被覆率» ではなく «その対象の太さ» で、linear で混ぜると
///   シルエットの縁で太さが 0 へ向かって溶ける。届く距離が縁だけ短くなり、
///   細い部位 (脚・触手) の輪郭が消える。色も背景の黒と混ざって濁る。
///
/// WHY 深度を見ないか:
///   壁の裏の輪郭を出すかどうかはマスクを描く側で済んでいる (ObjectMaskSkinned.hlsl が
///   遮蔽された面を discard する。出す / 出さないは申告側の visibleOnly が決める)。
///   ここで二重に判定すると、貫通で出すと決めた輪郭がこちらで消える。
///
/// WHY 方向を放射状に舐めるか (正方形の全走査ではなく):
///   SelectionOutline.hlsl は半径 16px で 1089 タップまで膨らむ。あちらは編集中の
///   1 体だけだが、こちらは戦闘中ずっと乗る。放射状なら太さに関係なくタップ数が
///   一定で、しかも «方向ごとに届く距離を変える» がそのまま放電の揺らぎになる。

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Platform/Backend.hlsli"

// NOTE: PostProcess 段 (LDR / Composite の後) で走る。
//
// WHY 加算ブレンドに頼らず自分で合成するか:
//   LDR 段のカスタムパスは blendMode を見ない。常に全画面を «描き直す» 実装で、
//   前段の画は t5 で渡される (ExecuteCustomPostProcessPass)。«足す分» だけを返すと
//   画面が輪郭以外まっ黒になる。エディタの選択輪郭 (SelectionOutline.hlsl) が
//   同じ段で同じ形 ─ 前段を読んで混ぜて返す ─ を採っていて、あちらは実際に映る。
//
// WHY HDR 段をやめたか:
//   ブルームと露出に乗せたいので SceneHDR + ADDITIVE で書いていたが、画面に
//   一切出なかった。«最前面に出す» ことの方が «滲む» ことより優先される。
//   代償として 1 を超える明るさは頭打ちになり、輪郭はブルームを拾わない。
Texture2D texScene       : register(TEX_GBUFFER0);
Texture2D texOutlineMask : register(TEX_GBUFFER1);

// 全画面フェッチなので clamp 必須 (s0 は DX12 では WRAP)。
SamplerState sampPoint : register(SAMPLER_POINT_CLAMP);

// 舐める方向の数と、1 方向あたりの刻み。
// 方向を増やすと輪郭が滑らかになり、刻みを増やすと «太さの段» が細かくなる。
static const int kDirections = 10;
// WHY 3 から増やしたか: 縁の «外側がどこで終わるか» は刻みの粗さで量子化される。
//     3 刻みだと Width を上げたときにギザギザが階段に見えてしまい、
//     «折れている» ではなく «解像度が粗い» と読まれる。
static const int kSteps      = 4;

/// 三角波 [0,1]。
///
/// WHY sin を使わないか: sin と fbm は丸いので «うねり» にしかならず、角が生えない。
///     折れ線に見せたいなら三角波を重ねる (WeaponTrail.hlsl で同じ結論に至っている)。
float Tri(float x)
{
    return abs(frac(x) - 0.5f) * 2.0f;
}

/// 縁の «その場所» ごとの幅の倍率。輪郭そのものをギザギザにする層。
///
/// WHY 方向ではなく縁の位置で振るか (ここが «輪郭がギザギザに見えるか» の分かれ目):
///   方向で振ると、1 画素から見た幅が向きごとに変わるだけで、縁に沿って眺めると
///   滑らかなままになる。同じ縁の点を見ている画素が同じ値を引くようにして初めて、
///   «縁の形» そのものが折れる。だから位置をセルへ量子化して引く。
///
/// WHY 地の模様と時間で引く分を混ぜるか:
///   毎フレーム全部引き直すと «形» ではなく «砂嵐» になる。角のある地の模様を
///   半分残すと、ギザギザの形が保たれたまま上を放電が這う。
float JagAt(float2 hitPx, float tick, float cellPx, float amount)
{
    const float2 cell = floor(hitPx / max(cellPx, 1.0f));
    const float  fixedPart = Tri(dot(cell, float2(0.37f, 0.71f)));
    const float  livePart  = Hash2D(cell + tick * 0.37f);
    const float  mixed     = fixedPart * 0.45f + livePart * 0.55f;
    return 1.0f + amount * (mixed - 0.5f) * 1.6f;
}

/// 方向ごとの «届く距離» の倍率。1 を中心に折れ線状に揺れる。
///
/// WHY 揺らすか: 全周が同じ幅だと «光っている輪郭» にしかならない。幅が方向ごとに
///     折れていて初めて «電気が這っている» に見える。伸びる/縮むの跳ねは
///     呼び出し側の放電 (量子化した抽選) が受け持つので、ここは地の揺らぎだけ。
float ArcReach(float angle, float phase, float crackle)
{
    const float slow = Tri(angle * 0.5f  + phase * 0.31f);
    const float fast = Tri(angle * 1.75f - phase * 1.10f);
    const float wave = slow * 0.55f + fast * 0.45f;
    return 1.0f + crackle * (wave - 0.5f) * 0.8f;
}

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

// 輪郭が乗らない画素は前段の画をそのまま返す。この段は «描き直す» ので、
// 黒を返すとその画素が黒く塗り潰される。
float4 PassThrough(float2 uv)
{
    return float4(texScene.SampleLevel(sampPoint, uv, 0).rgb, 1.0f);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    const float radiusPx  = max(customParameters.x, 1.0f);
    const float crackle   = saturate(customParameters.y);
    const float speed     = max(customParameters.z, 0.0f);
    const float gain      = max(customParameters.w, 0.0f);
    const float arcRate   = max(customParameters2.x, 0.0f);
    const float arcChance = saturate(customParameters2.y);
    const float arcReach  = max(customParameters2.z, 1.0f);
    const float arcSharp  = max(customParameters2.w, 1.0f);

    // シルエットの内側は素通し。中を塗ると «光っている敵» になり、輪郭で
    // 位置と数を数えるという役目から外れる。
    const float4 center = texOutlineMask.SampleLevel(sampPoint, p.uv, 0);
    if (center.a > 0.0f) return PassThrough(p.uv);

    // 方向の刻みが揃うと、輪郭に kDirections 個の «角» が出る。画素ごとに位相を
    // ずらして角を散らす。
    //
    // WHY 時間を混ぜないか: 毎フレーム散らし方が変わると、輪郭の «形» そのものが
    //     ちらつく。動いてほしいのは放電 (ArcReach の位相) であって、輪郭の
    //     滑らかさではない。ここは画素に固定する。
    const float dither = Hash2D(p.uv * screenSize);
    // WHY 位相を巻き取るか: Hash2D は frac(sin(x)) で、引数が数千に達すると分布が
    //     壊れて縞が出る。時計をそのまま渡すとすぐその領域へ入る。周期 1 の整数で
    //     巻けば sin(phase * TWO_PI) の連続性も保てる。
    const float phase  = frac(time * speed / 128.0f) * 128.0f;

    // 位相を画面のブロック単位でずらす。
    //
    // WHY 場所で変えるか: 位相が時間だけで決まると、盤面の全員が同じ方向へ同じ
    //     タイミングで放電する。«それぞれが帯電している» ではなく «画面全体が
    //     1 つの効果» に見えてしまう。画素ごとにすると今度は縁がざらつくだけなので、
    //     輪郭を跨ぐ程度の粗さ (数十 px) で区切る。
    const float regionPhase = phase + Hash2D(floor(p.uv * screenSize / 24.0f)) * 8.0f;

    // 放電の抽選をどの «瞬間» で引くか。
    //
    // WHY 時刻を量子化するか: 連続時間で引くと毎フレーム抽選が変わり、«放電» ではなく
    //     «画面のちらつき» になる。数フレーム保つ粗さにして初めて筋として読める。
    const float arcTick = floor(regionPhase * arcRate);

    // ギザギザの粗さは輪郭の太さから出す。
    //
    // WHY ツマミを増やさないか: 太い輪郭に細かすぎる刻みを乗せるとただのノイズに、
    //     細い輪郭に粗い刻みを乗せると «途切れた線» に見える。要は太さに対する比で
    //     決まる値なので、独立した数字にすると必ず片方を触るたびに合わせ直しになる。
    const float jagCellPx = max(radiusPx * 0.9f, 2.0f);

    float  band      = 0.0f;
    float3 bandColor = float3(0.0f, 0.0f, 0.0f);
    float  bandArc   = 0.0f;

    [loop]
    for (int d = 0; d < kDirections; ++d)
    {
        const float angle = (float(d) + dither) * (TWO_PI / float(kDirections));
        const float2 dir  = float2(cos(angle), sin(angle));

        // 方向ごとに «今この瞬間そこが放電しているか» を引く。全周が同時に伸びると
        // 太い輪郭に戻るので、当たるのは一部だけにする。
        const float arcing = Hash2D(float2(float(d) + 0.5f, arcTick)) < arcChance
            ? 1.0f : 0.0f;
        const float stretch = lerp(1.0f, arcReach, arcing * crackle);
        const float reach = max(ArcReach(angle, regionPhase, crackle) * stretch, 0.05f);

        [loop]
        for (int s = 1; s <= kSteps; ++s)
        {
            const float t    = float(s) / float(kSteps);
            const float dist = radiusPx * t;
            const float2 uv  = p.uv + dir * dist * texelSize;

            const float4 m = texOutlineMask.SampleLevel(sampPoint, uv, 0);
            if (m.a <= 0.0f) continue;

            // 届く距離はその対象自身の太さ (m.a) が決める。1 本の共有幅にすると、
            // 大きいボスと小さい雑魚が同じ px 数で縁取られて «近さ» が読めなくなる。
            //
            // 触れた «縁の点» でギザギザを引く。同じ点を見ている画素どうしが同じ値を
            // 引くので、縁に沿って折れた形になる (JagAt の WHY)。
            const float jag = JagAt(uv * screenSize, arcTick, jagCellPx, crackle);
            const float limit = radiusPx * m.a * reach * jag;
            if (dist > limit) continue;

            const float strength = 1.0f - dist / limit;
            if (strength <= band) continue;

            band      = strength;
            bandColor = m.rgb;
            bandArc   = arcing;
        }
    }

    if (band <= 0.0f) return PassThrough(p.uv);

    // 明滅。ゆっくりした脈が «帯電している» を、量子化したちらつきが «放電している» を
    // 受け持つ。後者は arcTick と同じ刻みで引く ─ 別々の速さで動かすと、伸びる瞬間と
    // 明るくなる瞬間がずれて «電気» にならない。
    const float pulse  = sin(phase * TWO_PI) * 0.5f + 0.5f;
    const float jitter = Hash2D(floor(p.uv * screenSize / 24.0f) + arcTick);
    const float flicker = lerp(0.72f, 1.0f, pulse) * lerp(1.0f, 1.45f, jitter * crackle);

    // 縁を «芯 + 裾» の 2 段にする。線形のままだと太さのわりに薄く、
    // 太くすると今度は板に見える。芯を細く強く出すと線として読める。
    const float core = band * band * band;
    const float glow = band * band;

    // 放電した方向にだけ、細く鋭い筋を «足す»。
    //
    // WHY 帯を削らずに足すか: 電気で帯を欠けさせると、強くするほど輪郭が虫食いになって
    //     «どの脚が何極か» が読めなくなる。芯と裾で 1 本の帯を先に成立させ、
    //     放電はその上へ重ねるだけにする (WeaponTrail.hlsl と同じ)。
    // saturate は «念のため» ではない。band は 1 - dist/limit なので値域は [0,1) だが、
    // それを保証しているのは上の early-out で、コンパイラーは追えない。
    // 負の底を渡す pow は未定義なので、警告 (X3571) を黙らせるより先に値域を明示する。
    const float filament = bandArc * pow(saturate(band), arcSharp);

    const float amount = saturate(customIntensity) * saturate(customBlend) * flicker * gain;

    // 筋だけ白へ寄せる。極の色のまま明るくすると «濃い赤/青» にしかならず、
    // 放電の «芯が白い» が出ない。色は帯が担当し、白は筋の量だけで足す。
    const float3 tint  = bandColor * (core * 1.6f + glow * 0.7f + filament * 1.2f);
    const float3 spark = float3(1.0f, 1.0f, 1.0f) * filament * 0.9f;

    // 前段の画へ自分で足す。この段は描き直しなので、足した結果を返すのが «加算» に当たる。
    const float3 scene = texScene.SampleLevel(sampPoint, p.uv, 0).rgb;
    return float4(scene + (tint + spark) * amount, 1.0f);
}
