/// @file DecalScorch.hlsl
/// @brief 焼け跡デカール。テクスチャ無しで焦げ・煤・縁の熱を手続き的に描く
/// @author Hasegawa Jin
/// @date 2026-08-23
///
/// WHY 手続きで描くか:
///     着弾の焼け跡は「輪郭がぼやけた円に、縁だけ熱が残る」という形が決まっている。
///     テクスチャで持つと解像度ぶんのメモリと 1 発ごとの見た目の固定を抱えるのに対し、
///     半径と柔らかさを公開しておけば .mat 1 枚で大小・新旧を作り分けられる。
///
/// WHY 真円をやめたか:
///     なぞりは 0.30m 間隔で焼け跡を «列» で置く (BeamScorchComponent の spacing)。
///     全部が同じ真円だと、線ではなく «同じスタンプが等間隔で並んだ» に見えて、
///     手で引いた線に見えない。輪郭を角度で崩し、個体ごとに位相をずらす。
///
/// WHY 熱と焦げを分けて時間で動かすか:
///     焼けた «瞬間» と焼けて «3 秒経った» が同じ絵だと、どの跡が今引いた線なのかが
///     読めない。跡が残ること自体は 6.2 の「地形には極性が乗らない」を補う表示なので、
///     新しさが読めないと «外した場所» の情報として機能しない。
///     時間は cooled / coverage を呼び出し側 (BeamScorchComponent) が毎フレーム
///     進めることで入る。デカールの cbuffer には時刻が無い。
///
/// 既定値の取り方: InitDefaultMaterialParams が全 float を 1.0 で敷くため、
///     「1.0 が自然な状態」になるようパラメータを選んである (coverage は 1 で満開、
///     cooled は 1 で冷え切った跡)。
#include "Material/Decal/DecalCommon.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"


cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 scorchColor;   // offset  0  焦げの色 (a = 全体の不透明度)
    float4 rimColor;      // offset 16  縁に残る熱の色
    float  edgeSoftness;  // offset 32  輪郭のぼけ幅 [0, 1]
    float  rimIntensity;  // offset 36  縁の加算量 (0 で縁なし)
    float  coverage;      // offset 40  1 で満開、0 で消滅。時間で縮めると鎮火に見える
    // 0 = 焼けた瞬間 (縁が白熱) / 1 = 冷え切った (rimColor だけが残る)。
    // WHY 「熱さ」ではなく「冷めた度合い」で持つか: 1.0 が既定値として敷かれるため、
    //     .mat に書き忘れた材質が «永久に白熱した跡» にならないようにする。
    float  cooled;        // offset 44
    float  edgeBreakup;   // offset 48  輪郭の崩し量。0 で真円
    float  sootSpread;    // offset 52  焦げの外へ広がる煤の量。0 で煤なし
    // 輪郭の崩れ方を個体ごとにずらす種。呼び出し側が跡 1 枚ごとに違う値を入れる。
    float  seed;          // offset 56
    uint   textureMask;   // offset 60  bit0 = albedo で焦げを変調する

    // bindless のテクスチャ添字。Material::Upload が毎フレーム書き込む。
    uint texAlbedoIndex;
};

// WHY cbuffer の後ろへ置くか: 添字フィールドを参照して初期化するため、
//     宣言はフィールドより後ろでなければならない。
/// @note 読むのは描画ごとの添字ブロック (b14)。cbuffer の添字フィールドは Inspector に枠を出すためだけに残す。
///       組み込みデカールは C++ の DecalMaterialCB (添字なし) を b2 に書き、デカールごとの
///       テクスチャ上書きも DrawCall::textures だけを差し替えるので、CB の添字は当てにならない。
FBZZ_TEX2D(texAlbedo, TEX_ALBEDO_SLOT);

DecalPixelInput VSMain(uint id : SV_VertexID)
{
    return DecalVertexMain(id);
}

// 角度に沿って周期 2π で閉じる崩し [-1, 1]。
//
// WHY 値ノイズを使わないか: 角度は ±π で折り返す。周期の合わない乱数を引くと
//     そこだけ輪郭が途切れ、跡に «継ぎ目» が 1 本入る。整数倍音の和なら必ず閉じる。
float ScorchWobble(float angle, float phase)
{
    return sin(angle * 3.0f + phase) * 0.50f
         + sin(angle * 5.0f + phase * 2.3f + 2.1f) * 0.30f
         + sin(angle * 9.0f + phase * 3.7f + 4.3f) * 0.20f;
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

    const float wobble = ScorchWobble(angle, seed * 6.2831853f);
    const float soft   = max(saturate(edgeSoftness), 1.0e-3f);
    // 崩しは半径への倍率。加算にすると小さい跡ほど形が壊れる (0.05m の跡に
    // 0.2 の凹凸を足すと輪郭が反転する)。
    const float radius = saturate(coverage) * saturate(1.0f + wobble * 0.22f * saturate(edgeBreakup));

    const float body = 1.0f - smoothstep(radius - soft, radius, dist);

    // 焦げの «外» に薄く残る煤。境界を 1 本にすると縁が刃物で切ったように硬くなる。
    // WHY 別の半径で描くか: body のぼけ幅を広げるだけだと焦げ本体まで薄くなり、
    //     «焼けた» ではなく «汚れた» に見える。濃い芯と薄い裾を独立に持たせる。
    const float sootRadius = radius * (1.0f + 0.5f * saturate(sootSpread));
    const float soot = (1.0f - smoothstep(sootRadius - soft * 2.0f, sootRadius, dist))
                     * saturate(sootSpread);

    // 落ち際の帯だけを取り出して縁の熱にする。body の山ではなく傾きが欲しいので、
    // body(1-body) を使う (中心と外側で 0、遷移帯の中央で最大)。
    const float rim = saturate(body * (1.0f - body) * 4.0f);

    // 焼けた直後は縁が白熱し、冷めるにつれて rimColor へ落ちて消える。
    // WHY 明るさではなく色から先に落とすか: 明るさだけを落とすと «暗い橙» になり、
    //     熱が引いたのではなく «暗いところで燃えている» ように見える。
    const float heat = saturate(1.0f - cooled);
    const float3 glowColor = lerp(rimColor.rgb, float3(1.0f, 0.94f, 0.82f), heat * heat);
    const float  glowGain  = rimIntensity * (1.0f + heat * 2.5f);

    float3 color = scorchColor.rgb + glowColor * rim * glowGain;
    float  alpha = scorchColor.a * saturate(body + soot * 0.30f) * surface.alpha;

    if (textureMask & 1u)
    {
        float4 texel = texAlbedo.Sample(sampDecal, surface.uv);
        color *= texel.rgb;
        alpha *= texel.a;
    }

    if (alpha < 0.001f)
        discard;

    return float4(color, alpha);
}