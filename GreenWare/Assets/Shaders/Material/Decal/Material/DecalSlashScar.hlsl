/// @file DecalSlashScar.hlsl
/// @brief 斬撃痕デカール。斬られた面へ残る «裂け目» を手続きで描く
/// @author Hasegawa Jin
/// @date 2026-09-06
///
/// WHY 相手の «体» に痕を残すか:
///     当たったことは今まで止め (ヒットストップ)・音・刃の白熱で伝えていたが、
///     どれも斬った «瞬間» にしか無い。振り抜いてしまえば画面は当たる前と同じで、
///     «自分がどこを何回斬ったか» が盤面に残らない。斬った線が体に残れば、
///     ボスの崩し (break-parry) がどこまで進んだかを HP バー以外からも読める。
///
/// WHY 焼け跡 (DecalScorch) と分けるか:
///     焼け跡は «輪郭がぼやけた円» で、斬撃痕は «両端が尖った 1 本の線»。形の芯が
///     違うので、同じシェーダーに両方を持たせると «円と線を切り替える分岐» が
///     増えるだけで、どちらの見た目も中途半端になる (BossTelegraph の shape 分岐が
///     まさにその形で、あれは予兆という 1 つの用途だから許容している)。
///
/// WHY 熱を «冷める» 側で持つか:
///     InitDefaultMaterialParams が全 float を 1.0 で敷くため、書き忘れた材質が
///     «永久に白熱した傷» にならない側を既定値にする (DecalScorch と同じ判断)。
///
/// 投影軸はデカールのローカル +Y。uv.x が斬った向き、uv.y がそれに直交する側で、
/// 呼び出し側 (SlashScarComponent) が箱の向きでそれを決める。
#include "Material/Decal/DecalCommon.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"


cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 scarColor;      // offset  0  裂け目そのものの色 (a = 全体の不透明度)
    float4 glowColor;      // offset 16  斬った直後に縁へ残る熱
    float  scarWidth;      // offset 32  裂け目の太さ [0,1]
    float  scarTaper;      // offset 36  両端の尖り。大きいほど細く長い線になる
    float  edgeSoftness;   // offset 40  縁のぼけ幅
    float  glowIntensity;  // offset 44  熱の加算量 (0 で熱なし)
    // 0 = 斬った瞬間 (縁が白熱) / 1 = 冷え切った (裂け目だけが残る)。
    float  cooled;         // offset 48
    float  coverage;       // offset 52  1 で満開、0 で消滅
    float  waver;          // offset 56  中心線の «ぶれ»。0 で定規で引いた直線
    float  seed;           // offset 60  1 枚ごとの揺らぎの種
    uint   textureMask;    // offset 64  bit0 = albedo で裂け目を変調する

    // bindless のテクスチャ添字。Material::Upload が毎フレーム書き込む。
    uint texAlbedoIndex;
};

FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);

DecalPixelInput VSMain(uint id : SV_VertexID)
{
    return DecalVertexMain(id);
}

// WHY frac(sin(...)) を使わないか: 引数が大きくなると float32 の引数簡約で下位ビットが
//     落ち、戻り値が乱数ではなく規則的な数列になる。ここは 1 枚ごとの種に係数を掛けて
//     渡すのですぐその領域へ入り、潰れたハッシュは «全部同じ形の傷» を作る。
float ScarHash(float2 cell)
{
    float3 p = frac(float3(cell.x, cell.y, cell.x) * float3(0.1031f, 0.1030f, 0.0973f));
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

float ScarNoise(float x, float lane)
{
    const float i = floor(x);
    const float f = frac(x);
    const float w = f * f * (3.0f - 2.0f * f);
    return lerp(ScarHash(float2(i, lane)), ScarHash(float2(i + 1.0f, lane)), w);
}

float4 PSMain(DecalPixelInput input) : SV_Target
{
    DecalSurface surface;
    if (!DecalResolve(input.screenUv, surface))
        discard;

    // uv.x = 斬った向きへの進み、uv.y = それに直交する側。中心を 0 に取り直す。
    const float along  = surface.uv.x;
    const float across = (surface.uv.y - 0.5f) * 2.0f;   // [-1, 1]

    // ── 中心線 ──────────────────────────────────────────────────────────────
    // WHY «ぶれ» を持たせるか: 定規で引いた直線は «刃が通った跡» ではなく
    //     «誰かが描いた線» に見える。刃は相手の面の凹凸を拾うので、必ず少し暴れる。
    const float lane   = seed * 37.0f;
    const float wobble = (ScarNoise(along * 4.0f + lane, lane) - 0.5f) * 2.0f * waver * 0.35f;
    const float offset = across - wobble;

    // ── 形 ──────────────────────────────────────────────────────────────────
    // 両端が尖った紡錘。sin を 0 で止めるのは、float の sin(PI) がわずかに負に出て、
    // 負の底の小数乗が NaN を返すため (末端の画素だけが消し飛ぶ)。
    const float lens = pow(max(sin(along * 3.14159265f), 0.0f), max(scarTaper, 0.05f));
    const float halfWidth = max(scarWidth, 1.0e-3f) * lens;

    const float soft = max(edgeSoftness, 1.0e-3f) * max(scarWidth, 1.0e-3f);
    // 裂け目そのもの。外へ向かって soft ぶんだけ溶ける。
    const float cut = 1.0f - smoothstep(halfWidth - soft, halfWidth + soft, abs(offset));

    // 縁の熱。裂け目の «すぐ外» が最も熱い ─ 中心を熱くすると «光る線» になって、
    // 面が割れているように見えない。
    const float rim = saturate(cut * (1.0f - cut) * 4.0f);

    const float open = saturate(coverage);
    const float heat = saturate(1.0f - cooled);

    // ── 色 ──────────────────────────────────────────────────────────────────
    float3 color = scarColor.rgb;
    float  alpha = cut * scarColor.a * open;

    // 熱は «足す» だけ。裂け目の暗さを熱で薄めると、冷めるにつれて傷が濃くなる
    // という逆立ちが起きる。
    color += glowColor.rgb * rim * max(glowIntensity, 0.0f) * heat;
    // 斬った直後だけ、熱の帯そのものも不透明度を持つ (面から光が漏れている状態)。
    alpha = saturate(alpha + rim * heat * 0.55f * open);

    if (textureMask & 1u) {
        const float3 sampled = texAlbedo.Sample(sampDecal, surface.uv).rgb;
        color *= sampled;
    }

    if (alpha <= 0.002f) discard;
    return float4(color, alpha);
}