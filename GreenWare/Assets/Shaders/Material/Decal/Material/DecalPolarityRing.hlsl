/// @file DecalPolarityRing.hlsl
/// @brief 極性の作用半径を地面へ描く環。照準中の «距離の表示» と、弾けた瞬間の放射に使う
/// @author Hasegawa Jin
/// @date 2026-08-28
///
/// WHY 環が要るか (Docs/weapon-emitter.md「照準中の表示は距離を出す」):
///   本作の芯は «反発 6m / 引力 12m» という距離の違いで、固めると弾け、散らすと集まる。
///   ところが 6m という距離は 3D の画面からは読めない。中和のプレビューを廃止した
///   代わりにプレイヤーへ渡すのは «結果» ではなく «今どういう配置か» で、
///   その唯一の表示がこの環になる。
///
/// WHY デカールで描くか:
///   板を 1 枚置くと、床の起伏・瓦礫・敵の足元でめり込んで切れる。デカールは深度から
///   受け面を復元して投影するので、環が地形に沿って «敷かれた» 形になる。
///   起伏のあるアリーナで «この距離» を示すには、面に貼り付いている必要がある。
///
/// WHY 実線ではなく破線 + 内向きの目盛りか:
///   ただの光る円は «エフェクト» にしか見えず、測っている値だという情報が無い。
///   等間隔の目盛りが付くと計器として読めるようになり、環の «内側と外側で
///   起きることが違う» という意味が形から伝わる。タイトルの磁場グリッド
///   (FieldGrid.hlsl) と同じ語彙でもある。
///
/// WHY 時刻を持たないか:
///   デカールの cbuffer に時刻が無い (DecalCommon.hlsli)。回転と明滅は呼び出し側が
///   spin / pulse を毎フレーム進めることで入る (BeamScorchComponent の cooled と同じ形)。
///
/// 2 つの使い方:
///   - 作用半径の表示  … sweep = 1 (掃きなし)。spin をゆっくり進めて «生きている» を出す
///   - 弾けた瞬間の放射 … sweep を 0 → 1 へ 0.15 秒で進める。環そのものは薄く
///
/// 既定値の取り方: InitDefaultMaterialParams が全 float を 1.0 で敷くため、
///     1.0 が «安全側» になるよう意味を選んである (dashRatio 1 = 実線、
///     sweep 1 = 掃き終わって見えない)。
#include "Material/Decal/DecalCommon.hlsli"

Texture2D texAlbedo : register(TEX_ALBEDO);

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 ringColor;     // offset  0  環の色 (a = 環の不透明度)
    float4 fillColor;     // offset 16  内側を薄く塗る色 (a = その不透明度)

    float  ringRadius;    // offset 32  環の位置。1.0 で OBB の外接円
    float  ringWidth;     // offset 36  環の太さ (半径 1 に対する比)
    float  edgeSoftness;  // offset 40  縁のぼけ [0,1]。内部で 0.15 倍して使う
    float  fillStrength;  // offset 44  内側の塗りの濃さ

    float  tickCount;     // offset 48  目盛りの数。0 で目盛りなし
    float  tickWidth;     // offset 52  目盛り 1 本の角度幅 [0,1]
    float  tickLength;    // offset 56  目盛りが環から内側へ伸びる長さ (半径比)
    float  spin;          // offset 60  目盛りの回転位相 [周]。呼び出し側が進める

    float  dashRatio;     // offset 64  環の実線率 [0,1]。1 で切れ目なし
    float  pulse;         // offset 68  全体の明るさ倍率。呼び出し側が明滅させる
    float  sweep;         // offset 72  放射の進み [0,1]。1 で掃き終わり (見えない)
    float  sweepWidth;    // offset 76  放射の帯の太さ (半径比)

    float  innerFade;     // offset 80  中心を抜く量。1 で中心が完全に透ける
    uint   textureMask;   // offset 84  bit0 = albedo で環を変調する
    float2 _matPad;       // offset 88
};

DecalPixelInput VSMain(uint id : SV_VertexID)
{
    return DecalVertexMain(id);
}

/// 角度に沿った «目盛りの帯» [0,1]。周期が閉じるので継ぎ目ができない。
///
/// WHY frac の三角波で作るか: 目盛りは等間隔の矩形なので、sin で作ると縁が甘くなり
///     本数が増えたときに «明るい輪» へ潰れる。三角波なら幅を角度で直接指定できる。
float RingTicks(float angle01, float count, float width, float aa)
{
    if (count < 0.5f)
        return 0.0f;

    // 1 目盛りぶんの区間 [0,1) の中心からの距離。
    const float phase = abs(frac(angle01 * count) - 0.5f) * 2.0f;
    const float half  = saturate(width);
    return 1.0f - smoothstep(saturate(half - aa), saturate(half + aa), 1.0f - phase);
}

float4 PSMain(DecalPixelInput input) : SV_Target
{
    DecalSurface surface;
    if (!DecalResolve(input.screenUv, surface))
        discard;

    // 投影 UV の中心からの距離。1.0 が OBB の外接円 = 指定した半径そのもの。
    const float2 offset = surface.uv - 0.5f;
    const float  dist   = length(offset) * 2.0f;

    // 角度は [0,1) の 1 周。spin をそのまま足せば «周» の単位で回せる。
    const float angle01 = frac(atan2(offset.y, offset.x) * 0.15915494f + spin);

    // WHY 画面微分 (fwidth) で縁の甘さを取らないか:
    //   DecalResolve が false を返した画素はこの上で discard 済み。捨てたレーンが
    //   混ざったクワッドの微分は仕様上未定義で、しかもデカールは «受け面の外» が
    //   毎フレーム動くシルエット境界そのものなので、環の縁 1 列が距離に関係なく
    //   ちらつく。デカールでは半径に対する固定比で甘さを決める
    //   (DecalScorch.hlsl も同じ理由で微分を使っていない)。
    const float soft = max(saturate(edgeSoftness) * 0.15f, 1.0e-3f);

    const float radius = saturate(ringRadius);
    const float width  = max(ringWidth * 0.5f, 1.0e-3f);

    // ── 環そのもの ────────────────────────────────────────────────────────
    // 中心からの «環までの距離» で帯を作る。太さは半径に依らず一定になる。
    const float toRing = abs(dist - radius);
    float band = 1.0f - smoothstep(width, width + soft, toRing);

    // 破線。実線率 1.0 で切れ目が消えるので、書き忘れた材質でも環は必ず出る。
    const float duty = saturate(dashRatio);
    if (duty < 0.999f)
    {
        const float dash = abs(frac(angle01 * max(tickCount, 1.0f)) - 0.5f) * 2.0f;
        band *= 1.0f - smoothstep(duty - 0.15f, duty + 0.15f, dash);
    }

    // ── 内向きの目盛り ────────────────────────────────────────────────────
    // 環から内側へ伸びる短い線。計器として読ませるための情報で、環の «内側» が
    // 意味を持つ領域だということを向きで示す。
    const float tickInner = radius - max(tickLength, 0.0f);
    const float tickBody  = (1.0f - smoothstep(radius, radius + soft, dist))
                          * smoothstep(tickInner - soft, tickInner, dist);
    const float ticks = RingTicks(angle01, tickCount, tickWidth, 0.08f) * tickBody;

    // ── 内側の塗り ────────────────────────────────────────────────────────
    // WHY 中心を抜くか: 塗りは «この範囲» を示すためのもので、自機や敵の足元を
    //     覆うためのものではない。中心まで濃いと、そこに居るものが読めなくなる。
    const float inside = 1.0f - smoothstep(radius - soft, radius, dist);
    const float fill   = inside * smoothstep(0.0f, max(innerFade, 1.0e-3f), dist)
                       * saturate(fillStrength);

    // ── 放射 (弾けた瞬間) ─────────────────────────────────────────────────
    // 中心から外へ抜ける 1 本の帯。sweep が 1 に達すると自然に消えるので、
    // 呼び出し側は «進めるだけ» でよく、消す処理を持たない。
    const float sweep01   = saturate(sweep);
    const float sweepBand = 1.0f - smoothstep(0.0f, max(sweepWidth, 1.0e-3f),
                                              abs(dist - sweep01));
    const float wave = sweepBand * (1.0f - sweep01) * (1.0f - sweep01);

    // ── 合成 ──────────────────────────────────────────────────────────────
    const float gain = max(pulse, 0.0f);

    // WHY 環と目盛りを最大値で合わせるか: 和にすると交差部だけが 2 倍の明るさになり、
    //     目盛りの根元に «点» が並んで見える。同じ 1 本の線として繋げたい。
    //
    // NOTE: この変数を line という名前にしないこと。line は HLSL のモディファイア語
    //       (point / line / triangle / lineadj / triangleadj / sample / linear / centroid …)
    //       で、識別子には使えない。DXC は «modifiers must appear before type» で弾き、
    //       エラーはその行だけでなく以降の使用箇所へも連鎖して出る
    //       (ElectricChargeCommon.hlsli の ChargeBar に同じ注意がある)。
    const float stroke = max(band, ticks * 0.85f);
    const float shape  = saturate(stroke + wave * 0.9f);

    float3 color = ringColor.rgb * (stroke + wave) * gain
                 + fillColor.rgb * fill * gain;
    float  alpha = saturate(shape * ringColor.a + fill * fillColor.a) * surface.alpha;

    if (textureMask & 1u)
    {
        const float4 texel = texAlbedo.Sample(sampDecal, surface.uv);
        color *= texel.rgb;
        alpha *= texel.a;
    }

    if (alpha < 0.002f)
        discard;

    return float4(color, alpha);
}
