/// @file    PlayerGhost.hlsl
/// @brief   回避中のプレイヤーに出る残像。無敵を «体そのもの» で伝える殻。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// PSO: SOLID_NOCULL + ALPHA_BLEND + DEPTH_READ
///
/// MeshTrailComponent.materialPath の .mat (render_path = "trail") が shader に
/// これを指すと、MeshTrailRenderPass が組み込み SkinnedMeshTrail.hlsl の代わりに差す。
/// 過去のボーン姿勢で描き直す定型は MeshTrailMaterial.hlsli が供給する。
/// 張り替えるのは DodgeAfterimageComponent で、プレイヤー以外には付かない。
///
/// WHY 塗り潰しでは «居た跡» に見えないか:
///   組み込みの残像は形の中まで同じ濃さで詰まっている。もう «そこに無い体» なのに
///   中身が詰まっていると、半透明なだけの実体が 6 体並んでいるように見え、
///   しかも重なった枚数ぶん濃くなって «団子» になる。手前を向いた面を抜いて輪郭を
///   残すと、体積を失った «形» になり、重ねても濁らない。
///
/// WHY 縁を別の色にするか:
///   本体の色 (プレイヤー色の緑) は «誰の残像か» を言っていて、ジャスト回避で
///   白熱するときも DodgeAfterimageComponent が trailColor 側を塗り替える。
///   縁はそれとは別に «速さ» を言う層なので、材質側に固定で持たせて分ける。
///
/// WHY 縞を «時間» ではなく高さで刻むか:
///   このパスは b5 (PostProcConstants) を束縛しないので time が来ない。そもそも
///   残像は置かれた瞬間の姿勢で固まっているものなので、縞が流れると «置いた像»
///   ではなく «生きている何か» に見える。世界の高さで刻めば、転がった軌跡に沿って
///   縞が並び、体が通った順序がそのまま読める。
#define FBZZ_MESHTRAIL_SKINNED
#include "Material/Effects/MeshTrailMaterial.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    // 縁の色。a は明るさで、1 を超えるぶんはブルームが拾う。0 で縁取りを切る。
    float4 rimColor;
    // フレネルの指数。大きいほど縁が細く鋭くなる。
    float  rimSharpness;
    // 内側に残すアルファの比。1 で従来どおり全面、0 で輪郭だけ。
    float  coreAlpha;
    // 縞の密度 [本/m]。0 で縞を切る。
    float  scanDensity;
    // 縞の深さ [0,1]。1 で縞の谷が完全に抜ける。
    float  scanDepth;
};

float4 PSMain(MeshTrailPSIn input) : SV_Target0
{
    float4 color = gMeshTrailTex.Sample(gSampler, input.uv) * trailColor;

    // WHY 面の向き (dot の符号) を捨てるか:
    //   残像は両面描画で、転がると裏返った面がそのまま出る。符号を残すと裏面だけ
    //   縁が消え、丸まった体の内側に穴が空いたように見える。
    float3 n = normalize(input.worldNormal);
    float3 v = normalize(cameraPos - input.worldPos);
    float  rim = pow(saturate(1.0f - abs(dot(n, v))), max(rimSharpness, 0.01f));

    color.a *= lerp(rim, 1.0f, saturate(coreAlpha));
    // a は «縁の強さ»。寄せる量と明るさの両方に効くので、0 にすれば縁取りが丸ごと消える。
    color.rgb = lerp(color.rgb, rimColor.rgb * (1.0f + rimColor.a), rim * saturate(rimColor.a));

    // 縞。輪郭は削らない ─ 形が欠けると «壊れたメッシュ» に見える。
    if (scanDensity > 0.0f)
    {
        const float phase = input.worldPos.y * scanDensity;
        const float resolved = 1.0f - smoothstep(0.2f, 0.5f, fwidth(phase));
        float band = 0.5f + 0.5f * sin(phase * 6.2831853f) * resolved;
        color.a *= lerp(1.0f, band, saturate(scanDepth) * (1.0f - rim));
    }
    color.a = saturate(color.a);
    clip(color.a - 0.002f);
    return color;
}
