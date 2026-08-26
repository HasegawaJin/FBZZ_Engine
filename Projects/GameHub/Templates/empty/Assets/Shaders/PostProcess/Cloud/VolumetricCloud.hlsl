// FBZZ Engine
// PostProcess/Cloud/VolumetricCloud.hlsl | PostProcess
// 事前ベイクした 3D ノイズ (Shape + Detail) を使う本格ボリューメトリック雲。
// 視線レイマーチ + 太陽方向ライトマーチ (セルフシャドウ) + 多重散乱近似 + HG 位相。
//
// WHY: 手続き型 FBM をステップ毎に回すと ALU ネックになるため、Nubis/Horizon と同様に
//      タイラブルな 3D ノイズを HW トライリニアでサンプルする。立体的な塊・パララックス・
//      光の回り込み (シルバーライニング) が安価に出せる。
#include "Rendering/CloudVolume.hlsli"
#include "Common/Space.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Platform/Backend.hlsli"

Texture2D<float> g_depth : register(TEX_DEPTH);
SamplerState sampDefault : register(SAMPLER_LINEAR_CLAMP); // 深度の全画面フェッチ (s0 は DX12 では WRAP)

static const float kEmptyStep = 2.0f; // 空白領域でのステップ倍率 (empty-space skip)

// 多重散乱近似 (Wrenninge) — オクターブごとに消散・寄与・位相の鋭さを弱めて足す。
// WHY: 単散乱だけだと雲の内側が真っ黒に落ちる。実際の雲は内部で光が何度も跳ね返って
//      影側まで回り込むため、減衰の弱い「ぼけた光」を重ねてその分を補う。
static const int   kMsOctaves      = 3;
static const float kMsAttenuation  = 0.5f;
static const float kMsEccentricity = 0.5f;

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

// 4π 正規化を外した HG。
// WHY: この積分は σs=σe (散乱アルベド 1) を前提に「太陽の放射輝度 = lightColor」で解いている。
//      PDF として 1/4π で割ると太陽項だけが 1/12.6 に沈み、環境光しか見えない雲になる。
float HenyeyGreenstein(float cosT, float g)
{
    float g2 = g * g;
    return (1.0f - g2) / pow(max(1.0f + g2 - 2.0f * g * cosT, 1e-4f), 1.5f);
}

// 前方 / 後方 2 ローブ。anisotropy を上げるほど太陽の周りへ光が集まる。
float CloudPhase(float cosT, float eccentricity)
{
    float g = clamp(cloudProfile.z, 0.0f, 0.95f) * eccentricity;
    return lerp(HenyeyGreenstein(cosT, g), HenyeyGreenstein(cosT, -g * 0.55f), 0.35f);
}

// 太陽方向の光学的深さから、多重散乱ぶんを含む到達光量を求める。
float SunEnergy(float opticalToSun, float cosT)
{
    float msContribution = saturate(cloudShading.w);
    float energy = 0.0f;
    float attenuation = 1.0f, contribution = 1.0f, eccentricity = 1.0f;
    [unroll]
    for (int n = 0; n < kMsOctaves; ++n)
    {
        energy += contribution * CloudPhase(cosT, eccentricity) * exp(-opticalToSun * attenuation);
        attenuation  *= kMsAttenuation;
        contribution *= msContribution;
        eccentricity *= kMsEccentricity;
    }
    return energy;
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    float ndcDepth = g_depth.Sample(sampDefault, p.uv).r;
    float sceneDepth = ndcDepth >= 0.9999f
        ? cloudNoise.w
        : distance(cameraPos, ReconstructWorldPos(p.uv, ndcDepth, invViewProjection));

    float3 farPos = ReconstructWorldPos(p.uv, 1.0f, invViewProjection);
    float3 rd = normalize(farPos - cameraPos);

    float t0, t1;
    if (!FBZZCloudSlabIntersect(cameraPos, rd, t0, t1))
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    t1 = min(t1, min(sceneDepth, cloudNoise.w));
    if (t1 <= t0)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    int   steps      = clamp((int)cloudWind.w, 8, 96);
    float density    = max(cloudLayer.z, 0.0f);
    float extinction = FBZZCloudExtinction();
    // WHY: 雲層の厚みだけを基準にステップ長を決めると、水平に近い視線では交差区間が厚みの
    //      数倍になり、固定反復上限の前に t1 へ到達せず雲の後半が欠ける。実際に見えている
    //      区間 (t1 - t0) を steps 分割する。
    float pathLength = max(t1 - t0, 0.0f);
    float stepLen = max(pathLength / (float)steps, 0.5f);
    int maxIterations = clamp((int)ceil(pathLength / stepLen), 1, 256);

    float2 windWorld = FBZZCloudWindOffset();
    float  lightLen  = length(lightDir);
    float3 sunDir    = lightLen > 1.0e-4f ? -lightDir / lightLen : float3(0.0f, 1.0f, 0.0f);
    // 雲の明るさは空ドームと揃える必要があるため skyDimmer を使う。
    // lightIntensity は地表のライティング用スケールで、空の見た目とは別軸。
    float3 lightCol  = lightColor * max(skyDimmer, 0.0f) * max(cloudShading.y, 0.0f) * cloudSunTint.rgb;
    float3 ambientTop = (ambientColor * 2.0f + lightCol * 0.2f)
                      * max(cloudLighting.y, 0.0f) * cloudAmbTint.rgb;
    float  ambientFloor = saturate(cloudAlbedo.w);
    float3 albedo    = cloudAlbedo.rgb;

    float cosT   = dot(rd, sunDir);
    float silver = pow(saturate(cosT), 4.0f) * max(cloudLighting.z, 0.0f);
    float powderStrength = saturate(cloudShading.z);

    // 手前を抜くフェード。ステップ位置 t が minDistance を越えてから fadeDistance かけて濃くなる。
    float nearStart = max(cloudRange.x, 0.0f);
    float nearFade  = max(cloudRange.y, 1.0e-3f);

    // バンディング抑制: 開始位置をピクセルごとに [0,1)*stepLen だけずらす。
    float dither = frac(sin(dot(p.uv, float2(12.9898f, 78.233f))) * 43758.5453f);
    float t = t0 + dither * stepLen;

    float transmittance = 1.0f;
    float3 scatter = 0.0f;

    // empty-space skip: 安価な Shape サンプルで空白を粗ステップ (×kEmptyStep) で飛ばし、
    // 雲付近のみ細ステップで detail + ライトマーチを行う。
    [loop]
    for (int i = 0; i < maxIterations && t < t1 && transmittance > 0.01f; ++i)
    {
        float3 wp = cameraPos + rd * t;

        float weather = FBZZCloudWeather(wp, windWorld);
        float base = FBZZCloudShape01(wp, windWorld, weather);
        if (base <= 0.001f) { t += stepLen * kEmptyStep; continue; }

        // WHY: カメラが雲層の高さまで上がると視線が雲の内部から始まり、1 ステップ目で
        //      透過率が飽和して画面全体が真っ白になる。手前を抜くと内部を通り抜けられる。
        float d = FBZZCloudErodeDetail(base, wp, windWorld) * density
                * saturate((t - nearStart) / nearFade);
        if (d > 0.001f)
        {
            float sigmaE    = d * extinction;
            float stepTrans = exp(-sigmaE * stepLen);

            float opticalToSun = FBZZCloudOpticalDepthToSun(wp, sunDir, windWorld, weather);
            // Beer-Powder: 太陽側の縁を暗く落として立体感を強調。
            float powder = lerp(1.0f, 1.0f - exp(-sigmaE * stepLen * 2.0f), powderStrength);

            float3 sun = lightCol * (SunEnergy(opticalToSun, cosT)
                                   + silver * exp(-opticalToSun * 0.25f)) * powder;
            // 環境光は雲の天面から届くので、層の下ほど暗くする。
            float hFrac = saturate((wp.y - cloudLayer.x) / max(cloudLayer.y - cloudLayer.x, 1.0f));
            float3 ambient = ambientTop * lerp(ambientFloor, 1.0f, hFrac);

            // σs = σe (散乱アルベド 1) として区間内の消散を解析積分する。
            // 1 - stepTrans がこの区間で散乱に回るエネルギーの割合そのもの。
            scatter += transmittance * (sun + ambient) * albedo * (1.0f - stepTrans);
            transmittance *= stepTrans;
        }
        t += stepLen;
    }

    // 水平線ぎわのフェード。
    // WHY: 視線を水平へ倒していくと、雲層に入る距離 t0 が伸びて Max Distance を越えた瞬間に
    //      雲が消える。その直前まで交差区間は不透明なので、地平線に沿って硬い切れ目が出る。
    //      Max Distance の手前 horizonFade 割ぶんで薄くしていき、切れ目を溶かす。
    float horizonFade = saturate(cloudRange.z);
    float horizonMask = horizonFade > 1.0e-4f
        ? 1.0f - smoothstep(cloudNoise.w * (1.0f - horizonFade), cloudNoise.w, t0)
        : 1.0f;

    float alpha = saturate(1.0f - transmittance) * horizonMask;
    return float4(scatter * horizonMask, alpha);
}
