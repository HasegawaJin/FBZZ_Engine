/// @file DecalCrack.hlsl
/// @brief 床の亀裂デカール。テクスチャ無しで放射状のひび割れを手続き的に描く
/// @author Hasegawa Jin
/// @date 2026-08-31
///
/// WHY 手続きで描くか:
///     亀裂は «中心から何本か放射して、先へ行くほど細くなる» という形が決まっている。
///     テクスチャで持つと 1 枚の絵が全部の亀裂で使い回され、衝撃波が円周に何十枚も
///     置く用途では «同じ模様が並んでいる» としか見えない。角度と本数を種で振れば、
///     .mat 1 枚で全部違う割れ方になる。
///
/// WHY 放射状に限るか (網目状の亀裂にしないか):
///     この亀裂は «そこを何かが叩いた» ことの跡で、力は 1 点から外へ抜ける。
///     網目にすると «元から傷んでいた床» に見えて、今起きた出来事の跡にならない。
///
/// WHY 伸びる途中を持つか (growth):
///     置いた瞬間に完成形が出ると «スタンプを貼った» に見える。中心から先端へ
///     伸びる 0.1 秒があるだけで «割れた» という出来事になる。伸ばすのは
///     呼び出し側 (BossShockwaveComponent) で、デカールの cbuffer には時刻が無い。
///
/// WHY 光を別に持つか (glowColor / heat):
///     割れた直後だけ内側が光り、冷めると黒い溝だけが残る。DecalScorch が
///     cooled で «焼けた瞬間と冷めた跡» を分けているのと同じ理由で、
///     どれが今できた亀裂かが読めないと «通り過ぎた跡» として機能しない。
///
/// 既定値の取り方: InitDefaultMaterialParams が全 float を 1.0 で敷くため、
///     「1.0 が自然な状態」になるようパラメータを選んである (growth は 1 で
///     伸びきった状態、heat は 1 だと光るので .mat 側で 0 を明示する)。
#include "Material/Decal/DecalCommon.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 crackColor;   // offset  0  溝の色 (a = 全体の不透明度)
    float4 glowColor;    // offset 16  割れた直後に溝の底で光る色
    float  crackWidth;   // offset 32  溝の太さ [UV]。0.02 前後が 1 本の線に見える
    float  branchCount;  // offset 36  放射する本数。丸めて使う
    float  growth;       // offset 40  0 = 中心だけ / 1 = 先端まで伸びきった
    float  heat;         // offset 44  0 = 冷えた溝だけ / 1 = 底が光る
    float  meander;      // offset 48  溝の蛇行量。0 で直線
    float  seed;         // offset 52  1 枚ごとに変える。角度と長さの散らし
    uint   textureMask;  // offset 56  未使用 (テクスチャを取らない)
};

DecalPixelInput VSMain(uint id : SV_VertexID)
{
    return DecalVertexMain(id);
}

// 種から [0,1) の擬似乱数。整数インデックス i ごとに違う値を返す。
//
// WHY sin ハッシュか: 亀裂は 1 枚あたり十数本で、質の高い乱数は要らない。
//     テクスチャも定数バッファの配列も持たずに済む方が、材質 1 枚で完結する。
float Hash11(float i, float phase)
{
    return frac(sin(i * 127.1f + phase * 311.7f) * 43758.5453f);
}

float4 PSMain(DecalPixelInput input) : SV_Target
{
    DecalSurface surface;
    if (!DecalResolve(input.screenUv, surface))
        discard;

    // 投影 UV の中心からの距離。1.0 が OBB の外接円。
    const float2 offset = surface.uv - 0.5f;
    const float  dist   = saturate(length(offset) * 2.0f);
    const float  angle  = atan2(offset.y, offset.x);

    const float phase = seed * 6.2831853f;
    const int   count = (int)clamp(round(branchCount), 1.0f, 12.0f);
    const float grow  = saturate(growth);

    // 一番近い溝までの «正規化した横ずれ»。0 で溝の芯、1 で溝の外。
    float nearest = 1.0e9f;

    [loop]
    for (int i = 0; i < count; ++i)
    {
        const float fi = (float)i;
        // 均等割りに乱数のずれを足す。完全な均等だと «星形の刻印» に見える。
        const float base = 6.2831853f * (fi + Hash11(fi, phase) * 0.75f) / (float)count;
        // 長さは本ごとに違う。全部同じだと «車輪のスポーク» になる。
        const float len  = lerp(0.45f, 1.0f, Hash11(fi + 17.0f, phase));

        // 蛇行。距離に沿って向きが揺れるので、溝が «たわんだ線» になる。
        const float wob = sin(dist * 11.0f + fi * 2.3f + phase) * 0.08f
                        + sin(dist * 23.0f - fi * 1.7f - phase) * 0.04f;
        const float dir = base + wob * meander;

        // 角度差を [-π, π] へ畳む。畳まないと ±π の境目に溝が 1 本走る。
        float da = angle - dir;
        da = atan2(sin(da), cos(da));

        // 芯からの «横» の距離。角度差 x 半径で、外側ほど同じ角度差でも遠くなる。
        const float lateral = abs(da) * dist;

        // 伸びていない先は溝が無い。先端では細くする (根本が太く、先が消える)。
        const float reach = len * grow;
        const float along = saturate(dist / max(len, 1.0e-3f));
        const float taper = saturate(1.0f - along * 0.85f);
        const float width = max(crackWidth, 1.0e-4f) * taper;

        // reach を越えた画素は «まだ割れていない» ので候補から外す。
        const float cut = (dist <= reach) ? 0.0f : 1.0e9f;
        nearest = min(nearest, lateral / max(width, 1.0e-4f) + cut);
    }

    // 芯から外へ 1 で消える。手前に短いぼけを入れて縁のジャギを消す。
    const float core = 1.0f - smoothstep(0.55f, 1.0f, nearest);
    if (core <= 0.001f)
        discard;

    // 中心の «砕けた» 部分。溝だけだと放射の交点に穴が空いて見える。
    const float hub = (1.0f - smoothstep(0.0f, max(crackWidth, 1.0e-4f) * 2.5f, dist)) * grow;
    const float mask = saturate(max(core, hub));

    // 底の光は «割れた直後» と «溝の奥» の両方で強い。先端まで同じに光ると
    // 線が発光した紐に見えるので、根本ほど強く出す。
    const float depth = saturate(1.0f - dist);
    const float glow  = saturate(heat) * depth * depth;

    float3 color = crackColor.rgb + glowColor.rgb * glow * glowColor.a;
    float  alpha = crackColor.a * mask * surface.alpha;

    if (alpha < 0.001f)
        discard;

    return float4(color, alpha);
}
