/// @file    GameVignette.hlsl
/// @brief   被弾・危機の «ゲームのビネット»。暗い縁 + 色の縁取り + 内へ流れる繊維 + 方向の張り出し
/// @author  Hasegawa Jin
/// @date    2026-09-06
///
/// customParameters
///   x — 暗さ 0..1。縁がどれだけ黒へ落ちるか
///   y — 半径 0..1。ここより内側は素通し (画面の «高さ» の半分に対する比)
///   z — 脈 0..1。1 で半径が最も内側へ縮む。危機 (残り HP 1) の鼓動に使う
///   w — 方向 [rad]。被弾した向きへ縁が張り出す。-10 以下で «向き無し»
/// customParameters2
///   xyz — 縁取りの色。暗い縁の内側に走る細い光の帯
///   w   — 方向の張り出しの強さ 0..1
/// customIntensity — 全体の包絡 0..1。スクリプトが実時間で作る
///
/// WHY エンジンのビネットを使わないか:
///   あちらは «暗い楕円» 1 枚で、被弾も低体力も同じ絵になる。ゲームの縁は
///   «どちらから来たか» (方向) と «今どれだけ危ないか» (鼓動) を言えなければ
///   ならず、それは色と時間と向きを持つ 1 本のパスでしか作れない。
///
/// WHY 暗さの内側に «色の帯» を置くか:
///   黒だけの縁は «画面が狭くなった» にしか見えない。暗さの境目に細い色の帯が
///   走ると、縁が «光っている輪» として読めて、暗い方は «その影» になる。
///   被弾の赤は極の＋ (純赤) と区別するため、スクリプト側が彩度を落とした色を渡す。
///
/// WHY 繊維を «内へ» 流すか:
///   縁が静止していると被弾の 0.3 秒に «掛かって、消えた» しか起きない。
///   角度ごとの筋が中心へ向かって流れると、外から何かが «入ってきた» 向きの
///   運動になり、押された感触 (Knockback) と同じ向きに画面が動く。
///
/// WHY 方向の張り出しを楕円の変形で作るか:
///   別の楔を足すと、楔と楕円の 2 枚が縁で段になる。半径そのものを角度で
///   変調すれば、張り出しも縁取りも繊維も全部が 1 つの形に乗ったまま動く。

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/Color.hlsli"
#include "Common/Random.hlsli"
#include "Platform/Backend.hlsli"

// t5 は RenderSystem が自動で束ねる LDR カラー。
Texture2D    texInput   : register(TEX_GBUFFER0);
// 全画面フェッチなので clamp 必須 (s0 は DX12 では WRAP)。
SamplerState sampLinear : register(SAMPLER_LINEAR_CLAMP);

static const float kPi = 3.14159265f;

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

/// 角度で刻んだ繊維。内側へ流れる。
///
/// WHY 2 層を重ねるか: 1 層だと «回る歯車» に見える。周期と速さの違う 2 層を
///     掛けると、筋どうしが互いに滑って «流れ» にしか見えなくなる。
float Fibers(float angle, float radial, float clock)
{
    const float a0 = angle / (2.0f * kPi);
    const float f0 = Hash2D(float2(floor(a0 * 48.0f), floor(radial * 6.0f - clock * 2.2f)));
    const float f1 = Hash2D(float2(floor(a0 * 90.0f + 0.37f), floor(radial * 11.0f - clock * 3.6f)));
    return saturate(f0 * 0.6f + f1 * 0.4f);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    const float3 orig = texInput.SampleLevel(sampLinear, p.uv, 0).rgb;

    const float dim        = saturate(customParameters.x);
    const float radiusBase = max(customParameters.y, 0.05f);
    const float pulse      = saturate(customParameters.z);
    const float hurtAngle  = customParameters.w;
    const float3 rimColor  = customParameters2.xyz;
    const float lobe       = saturate(customParameters2.w);

    // 画面比を戻した中心からの距離。UV のままだと縁が横長の楕円にならない
    // (画面は横長なので、左右の縁が上下より «遠く» にあるのが自然)。
    const float aspect = screenSize.x / max(screenSize.y, 1.0f);
    float2 centered = (p.uv - 0.5f) * 2.0f;
    centered.x *= aspect;
    // 楕円は «横長» に保つ。aspect を掛けたままだと四隅だけ真っ黒になる。
    const float2 shaped = float2(centered.x / max(aspect, 1.0f) * 1.15f, centered.y);
    const float dist    = length(shaped);
    // 画面の上が +y になる向きで角度を測る (UV の y は下向き)。
    const float angle   = atan2(-centered.y, centered.x);

    // 方向の張り出し。被弾した向きだけ半径が内側へ食い込む。
    float lobeIn = 0.0f;
    [branch]
    if (hurtAngle > -9.0f && lobe > 0.0f) {
        const float toward = cos(angle - hurtAngle);          // 1 = その向き
        lobeIn = pow(saturate(toward), 3.0f) * lobe * 0.30f;
    }

    // 鼓動で内側へ縮む。脈は 0 (平常) → 1 (最も縮む)。
    const float radius = radiusBase * (1.0f - pulse * 0.16f) - lobeIn;

    // 縁の «深さ» 0..1。半径から画面の端まで滑らかに。
    const float edge = saturate((dist - radius) / max(1.35f - radius, 0.05f));
    const float soft = edge * edge * (3.0f - 2.0f * edge);

    // 繊維。縁の深さで濃くなり、内へ流れる。
    const float fiber = Fibers(angle, dist, time) * soft;

    // 暗さ。繊維が乗った所は少し余計に暗い。
    const float darkness = saturate(soft * dim * (0.85f + fiber * 0.35f));

    // 縁取り。暗さが立ち上がる場所に細い帯。深さ 0.08〜0.28 に山を置く。
    const float band = saturate(1.0f - abs(soft - 0.18f) / 0.14f);
    const float rim  = band * band * dim * (0.55f + fiber * 0.45f);

    // 縁は彩度も落とす。«押された» 側が色を失う方が、赤を足すより «痛い» に読める。
    const float  luma = Luminance(orig);
    float3 result = lerp(orig, luma.xxx, soft * dim * 0.45f);
    result *= 1.0f - darkness;
    result += rimColor * rim;

    const float3 effected = lerp(orig, result, saturate(customIntensity));
    return float4(lerp(orig, effected, saturate(customBlend)), 1.0f);
}
