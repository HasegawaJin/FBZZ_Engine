/// @file    PolarityImplode.hlsl
/// @brief   集束の起点へ画面ごと引き込む。7.9 の「一斉に集束」を画面側で受ける
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// customParameters
///   x, y — 起点の UV (左上原点)。ScreenEffectManagerComponent が毎フレーム投影し直す
///   z    — 最大変位。画面の «高さ» に対する比で持つ
///   w    — 効いている半径。同じく画面の高さに対する比
/// customIntensity — 0..1 の包絡。立ち上がりと減衰はスクリプトが実時間で作る
///
/// WHY 引く向きにしか動かさないか:
///   外へ膨らませると uv が画面の外へ出る。Composite が画面外を黒で返すので
///   (ScreenEffectManagerComponent の surgePull と同じ理由)、起爆のたびに四隅が欠ける。
///
/// WHY ぼかしを歪みと同じベクトルで作るか:
///   «引き込まれている» は静止した歪みだけでは出ない。動いた軌跡が残って初めて
///   速度に見える。変位そのものをぼかしの向きと長さに使えば、歪みと尾が必ず
///   一致するうえ、調整するパラメーターが 1 つ減る。

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/Color.hlsli"
#include "Platform/Backend.hlsli"

// t5 は RenderSystem が自動で束ねる LDR カラー。
Texture2D    texInput   : register(TEX_GBUFFER0);
// 全画面フェッチなので clamp 必須 (s0 は DX12 では WRAP)。
// 起点が画面外にあるとき、尾のタップは画面の外を舐めにいく。clamp なら縁の色が
// 伸びるだけで済むが、wrap だと反対側の絵が回り込んで «裂け目» になる。
SamplerState sampLinear : register(SAMPLER_LINEAR_CLAMP);

// 尾を作るタップ数。増やすほど滑らかになるが、全画面 × この数だけ帯域を食う。
static const int kStreakTaps = 6;

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

/// 中心と外縁で 0、その間で最大 1 になる山。
///
/// WHY 中心を 0 にするか:
///   起点の画素に変位が残ると、そこだけ «自分とは違う場所» を引いてくる。
///   1 画素ぶんの特異点は、集束の的がちょうど画面の中央に来たとき必ず目に入る。
float ImplodeFalloff(float normalizedDistance)
{
    const float u = saturate(normalizedDistance);
    // 27/4 を掛けると頂点 (u = 1/3) がちょうど 1 になる。
    return u * (1.0f - u) * (1.0f - u) * 6.75f;
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    const float3 orig = texInput.SampleLevel(sampLinear, p.uv, 0).rgb;

    const float2 center  = customParameters.xy;
    const float  maxPull = customParameters.z;
    const float  reach   = max(customParameters.w, 1.0e-3f);

    // 画面比を戻してから測る。UV のままだと半径が横長の楕円になり、
    // 起点が画面の左右端にあるときだけ渦が潰れて見える。
    const float aspect = screenSize.x / max(screenSize.y, 1.0f);
    float2 delta = p.uv - center;
    delta.x *= aspect;

    // NOTE: distance は HLSL の組み込み関数名。変数に使うと隠してしまう。
    const float dist    = length(delta);
    const float falloff = ImplodeFalloff(dist / reach);

    float3 result = orig;
    [branch]
    if (falloff > 0.0f && maxPull > 0.0f && dist > 1.0e-5f) {
        // 起点へ向かう変位。測るのに掛けた画面比を、UV へ戻すときに割る。
        float2 pull = -(delta / dist) * (maxPull * falloff);
        pull.x /= aspect;

        // 変位の 0 から端までを等間隔に舐める = 歪みの軌跡がそのままぼけになる。
        const float2 stride = pull / float(kStreakTaps);
        float3 sum = float3(0.0f, 0.0f, 0.0f);
        [unroll]
        for (int i = 0; i < kStreakTaps; ++i)
            sum += texInput.SampleLevel(sampLinear, p.uv + stride * float(i), 0).rgb;
        result = sum / float(kStreakTaps);

        // 尾に沿ってわずかに持ち上げる。ぼかしただけだと «速く動いた» ではなく
        // «ピントが外れた» に見える。12.2 があるので色は足さず明るさだけを触る。
        result *= 1.0f + falloff * 0.30f;
    }

    const float3 effected = lerp(orig, result, saturate(customIntensity));
    return float4(lerp(orig, effected, saturate(customBlend)), 1.0f);
}
