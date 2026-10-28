/// @file    WaterStandardAppearance.hlsli
/// @brief   Default water pixel appearance, reusable by a project without duplicating shared surface geometry.
/// @author  Hasegawa Jin
/// @date    2026-10-21
#ifndef FBZZ_WATER_STANDARD_APPEARANCE_HLSLI
#define FBZZ_WATER_STANDARD_APPEARANCE_HLSLI
/// @note ユーザー定義エフェクトパラメータ。MaterialComponent.paramData にマップされ、
/// @note Script から mc->SetParam<float>("rimGlowStrength", val) で動的に書き換えられる。
/// @warning HLSL cbuffer のパッキング規則に従い 16B 境界を揃えること (48 bytes = float4 x3)。
cbuffer MaterialConstants : register(b2)
{
    float  rimGlowStrength;    ///< @brief Row0: 浅い角度で水の層が明るむ量 (光を受ける) default 0.40
    float  minShallowAlpha;    ///< @brief 水深 0 でも残る濁り [0,1] (opacity と積) default 0.65
    float  specularStrength;   ///< @brief スペキュラー強度   default 0.75
    /// @note 旧 specularExponent。ハイライトの鋭さは smoothness と specular AA から導出するため
    /// @note 廃止した。両方から指数を決めると、遠景でハイライトが 1 ピクセルに縮んで這う。
    float  _pad0;
    float3 skyReflectTint;     ///< @brief Row1: 空反射ベース色 RGB default (0.45, 0.82, 1.0)
    /// @note 旧 envMapBlend。空反射の混合率は skyReflection (g_reflectParams.x) が持つ。
    /// @note 同じ役目のつまみが 2 つあり、片方だけ動かしても効かない状態だった。
    float  _pad1;
    float3 rippleRingColor;    ///< @brief Row2: 波紋リング色 RGB   default (0.88, 0.97, 1.0)
    float  rippleRingStrength; ///< @brief 波紋リング強度     default 0.72
}

float4 PSMain(WaterPSInput p) : SV_Target0
{
    bool splineSurface = p.splineSurface.w > 0.5f;
    if (splineSurface) {
        float3 triangleNormal = cross(ddy(p.worldPos), ddx(p.worldPos));
        float normalLengthSq = dot(triangleNormal, triangleNormal);
        if (normalLengthSq > 1.0e-8f) {
            triangleNormal *= rsqrt(normalLengthSq);
            if (dot(triangleNormal, p.normal) < 0.0f) triangleNormal = -triangleNormal;
            p.normal = triangleNormal;
        }
    }
    if (WaterSurfaceExcluded(p)) discard;
    /// @note ワイヤーは変形後のメッシュを診断する。屈折・SSR・透明度・外周フェードを通すと線が背景へ溶け込むため、不透明な診断色で描く。
    if (waterWireframeMode > 0.5f) {
        const float3 wireNormal = normalize(p.normal);
        const float diffuse = abs(dot(wireNormal, normalize(-lightDir)));
        const float lighting = waterWireframeMode > 1.5f ? 1.0f
            : max(0.25f, saturate(diffuse * lightIntensity + dot(ambientColor, float3(0.2126f, 0.7152f, 0.0722f))));
        return float4(float3(1.0f, 0.65f, 0.1f) * lighting, 1.0f);
    }
    float time = g_timeParams.w;
    float2 screenUV = p.screenPos.xy / p.screenPos.w * float2(0.5f, -0.5f) + 0.5f;

    /// @note 1 ピクセルが覆うワールド距離。さざ波の LOD・タイルの異方フィルタ・specular AA が
    /// @note すべてこれを基準にする。
    float2 footprintDX = ddx(p.worldPos.xz);
    float2 footprintDY = ddy(p.worldPos.xz);
    float  footprint   = max(length(footprintDX), length(footprintDY));
    float2 baseDX = ddx(p.baseWorldXZ);
    float2 baseDY = ddy(p.baseWorldXZ);

    /// @note 場の評価は頂点で行う。補間した向きは渦の中心でゼロになり得るため安全に正規化する。
    WaterFlowSample flowSample;
    float flowLength = length(p.flowDirectionDetail.xy);
    flowSample.direction = flowLength > 1.0e-4f ? p.flowDirectionDetail.xy / flowLength : g_flowParams.xy;
    flowSample.detailGain = p.flowDirectionDetail.z;
    flowSample.chop = p.flowDirectionDetail.w;
    flowSample.eye = p.flowFoam.x;
    flowSample.ring = p.flowFoam.y;
    float2 authoredFlow = p.splineSurface.yz;
    float authoredFlowSpeed = length(authoredFlow);
    if (splineSurface && authoredFlowSpeed > 1.0e-4f)
        flowSample.direction = authoredFlow / authoredFlowSpeed;
    float2 flowDir = flowSample.direction;
    float2 flowTravel = splineSurface
        ? authoredFlow * time
        : flowDir * (g_detailParams.y * time);

    float4 normalSample  = SampleWaterNormal(p.worldPos.xz, p.uv, flowDir, flowTravel, time, footprint,
                                             footprintDX, footprintDY, p.normal,
                                              flowSample.detailGain, p.meshCellSize, p.baseWorldXZ, baseDX, baseDY, splineSurface);
    float3 tangentNormal = normalSample.xyz;
    float  lostSlopeVariance = normalSample.w;

    float3x3 tbn;
    if (splineSurface) {
        float3 tangent = float3(1.0f, 0.0f, 0.0f) - p.normal * p.normal.x;
        if (dot(tangent, tangent) <= 1.0e-8f)
            tangent = float3(0.0f, 0.0f, 1.0f) - p.normal * p.normal.z;
        tangent = normalize(tangent);
        tbn = float3x3(tangent, normalize(cross(tangent, p.normal)), normalize(p.normal));
    } else {
        tbn = float3x3(normalize(p.tangent), normalize(p.binormal), normalize(p.normal));
    }
    float3 N = normalize(mul(tangentNormal, tbn));
    float3 V = normalize(cameraPos - p.worldPos);
    bool useCoefficientOptics = g_opticalParams.x > 0.5f;
    /// @note 平行投影の視線は画素位置によらない。係数光学の光路と位相で透視のeye-rayを使わない。
    if (isOrthographic > 0.5f && g_opticalParams.x > 0.5f)
        V = -normalize(float3(view[0][2], view[1][2], view[2][2]));
    /// @note 水面下から見上げているときは面の裏側を見ている (PSO は両面描画)。
    /// @warning 向きの判定に N を使うと、浅い角度のピクセルだけがばらばらに反転して斑になる。
    /// @note 決定は滑らかな幾何法線に任せる。
    bool cameraUnderwater = dot(p.normal, V) < 0.0f;
    if (cameraUnderwater) N = -N;
    float NdotV = saturate(dot(N, V));

    /// @note F0 は垂直入射の反射率。反射率を倍増すると浅い角度で飽和し、水中の散乱光が消える。
    /// @see https://doi.org/10.1111/1467-8659.1330233 Schlick, "An Inexpensive BRDF Model for Physically-based Rendering" (1994)
    float f0 = saturate(g_surfaceParams.z);
    float fresnel = f0 + (1.0f - f0) * pow(saturate(1.0f - NdotV), max(g_surfaceParams.w, 1.0f));
    fresnel = saturate(fresnel * g_surfaceParams.y);
    float3 refractedViewRay = float3(0.0f, 0.0f, 0.0f);
    if (useCoefficientOptics && cameraUnderwater)
    {
        /// @note Water-to-air Fresnel and Snell use the interior-to-exterior IOR ratio; exact TIR keeps full reflection.
        /// @see https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission PBRT v4 §9.3.5, unpolarized dielectric Fresnel and total internal reflection.
        const float waterToAirEta = 1.333f;
        float cosIncidentWater = saturate(NdotV);
        float sinTransmittedSquared = waterToAirEta * waterToAirEta
                                    * max(1.0f - cosIncidentWater * cosIncidentWater, 0.0f);
        if (sinTransmittedSquared >= 1.0f)
        {
            fresnel = 1.0f;
        }
        else
        {
            float cosTransmittedAir = sqrt(max(1.0f - sinTransmittedSquared, 0.0f));
            float sNumerator = waterToAirEta * cosIncidentWater - cosTransmittedAir;
            float sDenominator = max(waterToAirEta * cosIncidentWater + cosTransmittedAir, 1.0e-6f);
            float pNumerator = cosIncidentWater - waterToAirEta * cosTransmittedAir;
            float pDenominator = max(cosIncidentWater + waterToAirEta * cosTransmittedAir, 1.0e-6f);
            float reflectance = 0.5f * (sNumerator * sNumerator / (sDenominator * sDenominator)
                                       + pNumerator * pNumerator / (pDenominator * pDenominator));
            fresnel = saturate(reflectance * g_surfaceParams.y);
            refractedViewRay = refract(-V, N, waterToAirEta);
            if (dot(refractedViewRay, refractedViewRay) <= 1.0e-8f)
                fresnel = 1.0f;
        }
    }

    float rawSceneDepth = g_sceneDepth.Sample(g_samplerClamp, screenUV).r;
    /// @note 深度が far plane に張り付く場所は Terrain / Mesh が無い背景ピクセルとして扱う。
    /// @note 背景の skydome 色を屈折色として読むと、水面が空そのものに溶けてしまうため、
    /// @note «底が見えない深い水» として描く。
    /// @note 深度は Reversed-Z なので背景 (クリア値のまま・空) は 0。
    float backgroundMask = rawSceneDepth <= 0.0f ? 1.0f : 0.0f;
    float linearSceneDepth = LinearizeDepth(rawSceneDepth);
    /// @note クリップ w は平行投影では 1。両投影とも同じ view Z で水層の距離を求める。
    float linearSurfDepth = max(mul(float4(p.worldPos, 1.0f), view).z, 0.0001f);
    float waterDepth = lerp(max(0.0f, linearSceneDepth - linearSurfDepth), g_deepColorDepth.w, backgroundMask);
    if (splineSurface) {
        float viewRayZ = abs(mul(float4(-V, 0.0f), view).z);
        float authoredDepthView = max(p.splineSurface.x, 0.0f)
            * viewRayZ / max(abs(V.y), 0.05f);
        waterDepth = min(waterDepth, authoredDepthView);
    }
    float depthFactor = smoothstep(0.0f, 1.0f, saturate(waterDepth / max(g_deepColorDepth.w, 0.0001f)));

    float shallowFactor = smoothstep(0.0f, 1.0f, saturate(waterDepth / max(g_shallowColorDepth.w, 0.0001f)));
    /// @note 色だけは «うねりの山ほど水の層が薄い» ことを織り込む。水底までの距離だけで決めると
    /// @note 波の山も谷も同じ色になり、うねりが «色の付いた板» にしか見えない。
    /// @note アルファと屈折の判定には素の depthFactor を使う (そちらは «底までの距離» の話)。
    float colorDepthFactor = saturate(depthFactor * (1.0f - p.waveState.x * 0.40f));
    /// @note 浅瀬色 / 深部色は «水の層が散乱して返す色» (反射率)。光はこの後で掛ける。
    float3 scatterAlbedo = lerp(g_shallowColorDepth.xyz, g_deepColorDepth.xyz, colorDepthFactor);

    /// @note 水底からの光は Beer–Lambert で減る。赤から先に吸われるのが «水の色» の正体なので、
    /// @note 深部色の色相を 1 深部深度あたりの透過率として使い、明るさは別に e^-1 ずつ落とす。
    /// @see https://doi.org/10.1364/AO.36.008710 Pope & Fry, "Absorption spectrum (380–700 nm) of pure water" (1997)
    float  deepDepth = max(g_deepColorDepth.w, 1.0e-4f);
    float  deepPeak  = max(max(g_deepColorDepth.x, g_deepColorDepth.y), max(g_deepColorDepth.z, 1.0e-4f));
    float3 deepHue   = max(g_deepColorDepth.xyz / deepPeak, 0.02f);
    float  opticalDepth = waterDepth / deepDepth;
    float3 transmittance = pow(deepHue, opticalDepth) * exp(-opticalDepth) * (1.0f - backgroundMask);
    /// @note 散乱で水の層そのものの色へ置き換わる割合。深部深度で 95%。
    /// @note opacity と minShallowAlpha は «濁り» の上限と下限として残す。
    float  scatterOpacity = 1.0f - exp(-3.0f * opticalDepth);
    float  murk = lerp(saturate(g_surfaceParams.x) * lerp(saturate(minShallowAlpha), 1.0f, scatterOpacity),
                       1.0f, backgroundMask);

    /// @note スクリーンスペース屈折は水面法線とスネル方向で HDR カラー参照 UV をずらす。水底ジオメトリを
    /// @note 再描画せず透明水面らしい歪みを得る。現在描画中の HDR RT を直接読むと read/write
    /// @note 競合になるため、Water 直前にコピーした sceneColor を参照する。
    float refractionMask = shallowFactor * (1.0f - backgroundMask);
    /// @note 水面を通る光は面の傾きと反対側へ曲がる。画面 UV の上下反転もカメラ空間で合わせる。
    /// @see https://www.graphics.cornell.edu/~bjw/microfacetbsdf.pdf Walter et al. (2007) §4.2 Ideal Refraction
    float3 viewNormal = mul(float4(N, 0.0f), view).xyz;
    float2 refrOffset = float2(-viewNormal.x, viewNormal.y) * g_refractionFlowParams.x
                      * (1.0f - fresnel) * refractionMask;
    if (useCoefficientOptics && cameraUnderwater && fresnel < 1.0f)
    {
        float3 incidentViewRay = mul(float4(-V, 0.0f), view).xyz;
        float3 transmittedViewRay = mul(float4(refractedViewRay, 0.0f), view).xyz;
        float2 incidentSlope = incidentViewRay.xy / max(abs(incidentViewRay.z), 0.05f);
        float2 transmittedSlope = transmittedViewRay.xy / max(abs(transmittedViewRay.z), 0.05f);
        float2 slopeDelta = transmittedSlope - incidentSlope;
        refrOffset = 0.5f * float2(slopeDelta.x * projection[0][0], -slopeDelta.y * projection[1][1])
                   * g_refractionFlowParams.x * (1.0f - fresnel);
    }
    float2 refrUV = saturate(screenUV + refrOffset);

    /// @note 屈折先が水面より手前 (= カメラと水面の間に物体がある) なら屈折させない。
    /// @note そのまま UV をずらすと手前オブジェクトのシルエットが水中に滲み出す。
    float refrRawDepth    = g_sceneDepth.Sample(g_samplerClamp, refrUV).r;
    float refrLinearDepth = LinearizeDepth(refrRawDepth);
    if (refrLinearDepth < linearSurfDepth)
        refrUV = screenUV;
    /// @note sceneColor はシーン側で照明・影が済んだ色なので、ここでは減衰だけを掛ける。
    float3 refractColor = g_sceneColor.Sample(g_samplerClamp, refrUV).rgb;

    /// @note 空反射は空連動 IBL の事前フィルタ済みキューブから引く (専用の環境テクスチャは不要)。
    /// @note 水面上からの反射だけを上半球へ保つ。水中から見た係数光学では下半球の反射を使う。
    /// @note 水平線際の上面反射はさざ波による下半球サンプルで黒帯にならないよう保守的に制限する。
    float3 R = reflect(-V, N);
    if (!(useCoefficientOptics && cameraUnderwater))
        R = normalize(float3(R.x, max(R.y, 0.02f), R.z));
    /// @note 消失割合でなく強度込みの傾き分散を GGX の alpha^2 へ足す。弱いさざ波まで遠景で一律に粗さ 0.7 へしない。
    /// @note 可視法線の画面内分散も含め、残した長いうねりのハイライトが subpixel へ縮む場合の明滅を抑える。
    /// @see https://www.jp.square-enix.com/tech/library/pdf/ImprovedGeometricSpecularAA.pdf Tokuyoshi & Kaplanyan (2019), filtered NDF roughness
    float  roughness = saturate(1.0f - g_detailParams.w);
    float3 normalDX = ddx(N);
    float3 normalDY = ddy(N);
    float normalVariance = 0.15f * (dot(normalDX, normalDX) + dot(normalDY, normalDY));
    float microfacetAlpha = roughness * roughness;
    float roughnessAA = sqrt(sqrt(saturate(microfacetAlpha * microfacetAlpha + min(2.0f * (lostSlopeVariance + normalVariance), 0.25f))));
    /// @note 寝た視線では粗さを引き戻して環境キューブを鮮明な mip から引く。粗い mip は上下 90 度ぶんを
    /// @note 平均した色で、水平線際では空に地面が混ざって沈む。実際の水面は入射が浅いほど反射ローブが
    /// @note 細くなるのでこちらが正しい。ハイライト側は 1 ピクセルのちらつきを避けるため据え置く。
    float  horizonSharpen = lerp(0.35f, 1.0f, NdotV);
    const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);
    /// @note IBL が無いときの空反射。.mat の空色を空の明るさ (skyDimmer) で落とす。Unlit 表示では
    /// @note skyDimmer が 0 になるので ambientColor (白) を下限にする。
    float3 reflectColor = skyReflectTint * max(skyDimmer, dot(ambientColor, kLuma));
    /// @note 水の層・泡が空から受ける放射照度 / π (Lambert の反射率に掛ければそのまま放射輝度)。
    float3 skyIrradianceOverPi = ambientColor;
    [branch]
    if (g_reflectParams.x > 0.001f && iblIntensity > 0.0f)
    {
        /// @note 通常マテリアルと同じく IBL の全体スケールと鏡面 / 拡散スケールを通す。
        float  iblSpecular = iblIntensity * max(iblSpecularScale, 0.0f);
        float3 sky = g_skyReflection.SampleLevel(g_samplerClamp, R,
                                                 roughnessAA * horizonSharpen
                                                     * (float)max(iblMaxMipLevel, 0)).rgb;
        /// @note 一番粗いミップを真上から引くと «上半球を余弦で平均した放射輝度» に近く、π 倍が
        /// @note 水平面の放射照度になる。LightConstants の ambientColor は時刻に追随しない。
        float3 skyAverage = g_skyReflection.SampleLevel(g_samplerClamp, float3(0.0f, 1.0f, 0.0f),
                                                        (float)max(iblMaxMipLevel, 0)).rgb;
        /// @note 空色は «色相» だけを .mat から取り、明るさは空から取る。定数のまま混ぜると夜も光る。
        float3 tintLit = skyReflectTint * (dot(skyAverage, kLuma)
                                           / max(dot(skyReflectTint, kLuma), 1.0e-3f));
        reflectColor = lerp(tintLit, sky, saturate(g_reflectParams.x)) * iblSpecular;
        skyIrradianceOverPi = skyAverage * (iblIntensity * max(iblDiffuseScale, 0.0f));
    }

    /// @note 反射ウェイト (フレネル) を先に求め、SSR は寄与が実際に見えるピクセルだけトレースする。
    /// @note TraceWaterSSR は WaterForward の主コストで、水面を見下ろす (低フレネル) ピクセルは
    /// @note レイマーチを丸ごと省いても結果はほぼ不変。背景ピクセルは画面内にヒット候補が無く
    /// @note 長い空走査になるため環境反射へフォールバックする。
    /// @warning 背景ピクセルで反射を減らさないこと。そこはフレネルが 1 に張り付き本来は鏡になる
    /// @note 水平線際で、以前の 0.45 倍で遠い水面が黒く沈んでいた。
    float reflectionWeight = saturate(fresnel);
    [branch]
    if (reflectionWeight > 0.04f && backgroundMask < 0.5f)
    {
        float4 ssrReflection = TraceWaterSSR(p.worldPos, N);
        /// @note Screen-color SSR is unfiltered; let roughness-AA IBL replace its sharp hit as fine slopes leave the pixel footprint.
        float ssrRoughnessWeight = saturate(1.0f - roughnessAA);
        reflectColor = lerp(reflectColor, ssrReflection.rgb, ssrReflection.a * ssrRoughnessWeight);
    }

    float3 L = normalize(-lightDir);
    /// @note 影は «太陽が届くか» だけを表す。掛けるのは太陽由来の項 (水の層・泡・きらめき・透過光) だけ。
    /// @warning 合成後の色全体へ掛けないこと。空の反射は影で暗くならず、屈折した水底は
    /// @note シーン側で影が済んでいるので二重に沈む。
    float shadow = ComputeShadow(g_shadowMap, g_shadowSampler, p.worldPos,
        lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    /// @note Forward の画面空間 AO / 接触影。Deferred では b8 が 0 なので素通りする。
    shadow *= FBZZ_ScreenContactShadow(p.svPosition.xy);

    /// @note 太陽の放射照度 / π。通常マテリアルと同じ LIGHT_UNIT_SCALE を通し、intensity=1 で
    /// @note 反射率そのままの明るさになる規約に揃える。
    float3 sunOverPi = lightColor * (max(lightIntensity, 0.0f) * LIGHT_UNIT_SCALE * INV_PI);
    /// @note 水の層へ入る太陽光は水平面で受けるので、さざ波の法線ではなく高度の余弦で決める。
    float  sunCosFlat = saturate(L.y);

    /// @note 局所光。水の層と泡を照らす放射照度 / π と、水面のきらめき (GGX) を分けて集める。
    /// @warning 屈折色へ拡散光を足さないこと。水底はシーン側で局所光も受け済みで、二重に照らされる。
    float3 punctualOverPi   = float3(0.0f, 0.0f, 0.0f);
    float3 punctualSpecular = float3(0.0f, 0.0f, 0.0f);
    float3 punctualSingleScatterSource = float3(0.0f, 0.0f, 0.0f);
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        float3 punctualRadiant = ps.color * (ps.intensity * LIGHT_UNIT_SCALE);
        float  punctualNdotL   = saturate(dot(N, ps.L));
        punctualOverPi   += punctualRadiant * (punctualNdotL * INV_PI);
        punctualSpecular += BRDF_Specular(N, V, ps.L, f0.xxx, saturate(roughnessAA + ps.roughnessBias))
                          * punctualRadiant * punctualNdotL;
        if (useCoefficientOptics && !cameraUnderwater) {
            float localPhase = WaterHenyeyGreensteinPhase(dot(-ps.L, V), g_opticalParams.y);
            punctualSingleScatterSource += punctualRadiant * (PI * localPhase);
        }
    FBZZ_PUNCTUAL_END

    float3 body;
    float3 bodyIrradiance = float3(0.0f, 0.0f, 0.0f);
    if (useCoefficientOptics) {
        if (cameraUnderwater)
        {
            /// @note Composite owns the camera-to-interface water path; the surface contributes only the refracted air-side radiance.
            body = refractColor;
        }
        else
        {
            /// @note waterDepth は view-Z の差なので、ray の view-space z 成分で割ってカメラ光路長 [m] を近似する。
            /// @note refracted UV の先にある実際の屈折経路は追跡せず、浅い視線の発散を 0.05 で抑える。
            float viewRayZ = abs(mul(float4(-V, 0.0f), view).z);
            float opticalPathLength = isOrthographic > 0.5f ? waterDepth : waterDepth / max(viewRayZ, 0.05f);
            WaterOpticalTransport opticalTransport = WaterEvaluateOpticalTransport(
                g_absorptionCoefficients.xyz, g_scatteringCoefficients.xyz, opticalPathLength);
            float phaseG = WaterOpticalClampPhaseG(g_opticalParams.y);
            float directPhase = WaterHenyeyGreensteinPhase(dot(-L, V), phaseG);
            /// @note sunOverPi is irradiance / π; multiplying by π·phase yields irradiance·phase without a second Lambert cosine.
            /// @note Isotropic phase 1/(4π) therefore contributes sunOverPi·0.25 without another Lambert term.
            float3 directScatterSource = sunOverPi * ((L.y > 0.0f ? 1.0f : 0.0f) * shadow * (PI * directPhase));
            /// @note Approximate the environment as uniform radiance over the visible hemisphere; isotropic integration gives one half.
            float3 ambientScatterSource = skyIrradianceOverPi * 0.5f;
            float3 inscatter = opticalTransport.singleScatterWeight
                             * (directScatterSource + ambientScatterSource + punctualSingleScatterSource);
            /// @note Rectangle water over cleared depth has no transmitted bottom; authored spline depth remains finite without terrain.
            /// @see https://dev.epicgames.com/documentation/unreal-engine/single-layer-water-shading-model-in-unreal-engine Epic Single Layer Water, homogeneous volume and scene-depth refraction inputs.
            float bottomTransmission = splineSurface ? 1.0f : (1.0f - backgroundMask);
            body = refractColor * opticalTransport.transmittance * bottomTransmission + inscatter;
        }
    } else {
        /// @note 水の層が返す光 = 散乱色 × (太陽 + 空 + 局所光)。定数色のまま出すと昼夜・影に追随しない。
        bodyIrradiance = sunOverPi * (sunCosFlat * shadow) + skyIrradianceOverPi + punctualOverPi;
        float3 inscatter = scatterAlbedo * bodyIrradiance;
        body = lerp(refractColor * transmittance, inscatter, murk);
    }

    /// @note 太陽のきらめきは GGX で引く。Blinn-Phong は裾が急に落ちるため、海のきらめきの
    /// @note «広がった尾» が出ず、ハイライトが «板に当たった光» に見える。
    /// @note 異方にするのは、さざ波が detailAnisotropy で風向と直交して伸びているから。同じ
    /// @note 異方を反射ローブへ渡すと、きらめきが風向に沿った帯になる ── 海面の見え方そのもの。
    /// @note 接空間は «風向» で張り直す。p.tangent はワールド +X 基準なので、そのまま渡すと
    /// @note 異方の向きが風と無関係になる。うねの筋を横切る «風向» 側が粗い。
    float3 flowWorld = float3(flowDir.x, 0.0f, flowDir.y);
    float3 specT = flowWorld - N * dot(N, flowWorld);
    float  specTLen = length(specT);
    specT = specTLen > 1.0e-4f ? specT / specTLen : normalize(p.tangent);
    float3 specB = normalize(cross(N, specT));
    float  specAniso = saturate((max(g_flowParams.z, 1.0f) - 1.0f) * 0.35f);

    /// @note 太陽は点ではなく角半径 0.0047 rad の円盤。α に円盤ぶんを足して峰を広げないと、
    /// @note GGX の頂点が 1 ピクセルへ落ちて Bloom がちらつく (球光源の正規化と同じ扱い)。
    /// @note GGX 分布は投影面積で正規化済み。粗さを広げた後に峰の比を全体へ掛けると裾の光まで失う。
    float sunAlpha = max(roughnessAA * roughnessAA, 1.0e-5f);
    float sunAlphaWide = saturate(sunAlpha + 0.0023f);
    float NdotL = saturate(dot(N, L));
    /// @see https://www.graphics.cornell.edu/~bjw/microfacetbsdf.pdf Walter et al., "Microfacet Models for Refraction through Rough Surfaces" (2007) §5.2 GGX
    float3 sunSpecular = BRDF_SpecularAdvanced(N, V, L, f0.xxx, sqrt(sunAlphaWide),
                                               specT, specB, specAniso)
                        * (NdotL * shadow * PI) * sunOverPi;

    if (!useCoefficientOptics) {
        /// @note 波の背面から透ける光 (subsurface)。«光を通す液体» に見えるかはここで決まり、
        /// @note 反射とスペキュラだけだと金属板のような水面になる。
        /// @note 光を法線で «曲げて» から視線と比べる (fast subsurface scattering)。素の dot(V,-L)
        /// @note だと太陽をちょうど背にした一瞬しか光らず、それ以外の角度で波の内側が常に暗い。
        float3 scatterDir = normalize(L + N * 0.30f);
        float forwardScatter = pow(saturate(dot(V, -scatterDir)), 3.0f);
        /// @note うねりの山ほど水の層が薄く、光が通り抜ける。
        float thickness = p.waveState.x * saturate(g_sssParams.w);
        /// @note 空からの散乱は影を受けない (空の光は影の中にも届く)。これが無いと曇りや夜の海だけが
        /// @note «光を通さない板» に戻る。
        body += g_sssParams.rgb * thickness
              * (sunOverPi * (forwardScatter * shadow) + skyIrradianceOverPi * 0.5f);

        /// @note 浅い角度では視線が水の層を長く通り、層の色が明るむ。自発光ではなく受けた光で出す。
        float rim = pow(1.0f - NdotV, 3.0f) * rimGlowStrength;
        body += lerp(scatterAlbedo, skyReflectTint, 0.35f) * bodyIrradiance * rim;
    }

    /// @note 透過した水の層と反射をフレネルで分け合い、表面のきらめきは上に足す。
    float3 color = lerp(body, reflectColor, reflectionWeight)
                 + (sunSpecular + punctualSpecular) * specularStrength;

    /// @note 波打ち際 (地形と水面が交差する浅瀬) に発生する接岸泡。waterDepth が threshold より
    /// @note 浅いほど強くし、岸辺に沿った白い帯を作る。
    float foamThreshold = g_foamParams.x;
    float foamFade      = max(g_foamParams.y, 0.0001f);
    float shoreFoam     = (1.0f - smoothstep(foamThreshold, foamThreshold + foamFade, waterDepth)) * (1.0f - backgroundMask);

    float foamMaskVal = 0.0f;
    if (g_terrainInfoParams.x > 0.5f) {
        float2 terrainUv = splineSurface ? p.uv : WaterTerrainUvAtWorldXZ(p.worldPos.xz);
        float2 terrainDepth = SampleWaterTerrainDepth(terrainUv);
        float verticalDepth = terrainDepth.y + p.worldPos.y - p.baseWorldY;
        float wet = verticalDepth > 0.0f ? 1.0f : 0.0f;
        float terrainFoam = (1.0f - smoothstep(foamThreshold, foamThreshold + foamFade,
            max(verticalDepth, 0.0f))) * wet;
        shoreFoam = lerp(shoreFoam, terrainFoam, saturate(terrainDepth.x));
    } else {
        foamMaskVal = g_foamMask.Sample(g_samplerClamp, p.uv).r;
    }
    float localDataWeight = WaterLocalDataWeight(p.uv);
    shoreFoam *= localDataWeight;
    foamMaskVal *= localDataWeight;
    /// @note 泡のムラもさざ波タイルから引く。ワールド座標なので水面の大きさに依らず粒が揃う。
    float foamScale = max(g_foamParams.w, 1.0e-4f);
    float2 foamAdvection = splineSurface
        ? -authoredFlow * (time * foamScale)
        : flowDir * (time * 0.35f);
    float2 foamP = p.worldPos.xz * foamScale + foamAdvection;
    /// @note 乱流 (Curl) はムラのコントラストを上げる。«ざわついた水» は泡の粒が揃わない。
    float foamTexVal = saturate(WaterNoiseTile(foamP, footprintDX * foamScale,
                                               footprintDY * foamScale).z
                                * lerp(1.6f, 3.0f, flowSample.chop));
    /// @note VS が波高と圧縮の被覆率を履歴込みで作る。岸泡の強度とは別に波頭だけを調整できる。
    float crestFoam = saturate(p.waveState.y * max(g_crestFoamParams.z, 0.0f));
    /// @note 渦と排水口の目は実際に白く泡立つ。足さないと «ただ凹んだ穴» にしか見えない。
    /// @note 湧き出しは縁で水がぶつかるので、輪として出す。
    float eyeFoam = max(flowSample.eye * 0.9f, flowSample.ring * 0.7f);
    float localFoamAmount = max(max(foamMaskVal, shoreFoam), eyeFoam) * foamTexVal;
    float localFoam = saturate(smoothstep(0.05f, 1.0f, localFoamAmount) * max(g_foamParams.z, 0.0f));
    float crestCoverage = smoothstep(0.05f, 1.0f, crestFoam * foamTexVal);
    float foam = saturate(max(localFoam, crestCoverage));
    /// @note 泡と波紋の輪は «水面に浮いた白い粗面» なので、定数色ではなく Lambert で照らす。
    /// @note 太陽は泡の面の向きで受け、影の中では空と局所光だけが残る。
    float3 surfaceIrradiance = sunOverPi * (NdotL * shadow) + skyIrradianceOverPi + punctualOverPi;
    [branch]
    if (foam > 0.001f) {
        float3 foamAlbedo = saturate(g_foamColorRoughness.rgb) * lerp(0.85f, 1.0f, saturate(foamTexVal));
        float foamRoughness = clamp(g_foamColorRoughness.w, 0.05f, 1.0f);
        float foamFresnel = 0.02f + 0.98f * pow(1.0f - NdotV, 5.0f);
        /// @note 泡は粗い誘電体として光を受ける。白色を直接加算せず、空と局所光の陰影も保持する。
        /// @see https://www.graphics.cornell.edu/~bjw/microfacetbsdf.pdf Walter et al. (2007), GGX dielectric reflection.
        float3 foamColor = foamAlbedo * surfaceIrradiance * (1.0f - foamFresnel);
        if (lightIntensity > 0.0f && NdotL > 0.0f) {
            foamColor += BRDF_Specular(N, V, L, float3(0.02f, 0.02f, 0.02f), foamRoughness)
                       * (NdotL * shadow * PI) * sunOverPi;
        }
        if (g_reflectParams.x > 0.001f && iblIntensity > 0.0f) {
            float3 foamSky = g_skyReflection.SampleLevel(g_samplerClamp, R,
                foamRoughness * (float)max(iblMaxMipLevel, 0)).rgb;
            foamColor += foamSky * (foamFresnel * iblIntensity * max(iblSpecularScale, 0.0f));
        }
        color = lerp(color, foamColor, foam);
    }

    float2 rippleRG = (g_rippleTex.Sample(g_samplerClamp, p.uv).rg * 2.0f - 1.0f) * localDataWeight;
    float rippleRing = saturate(length(rippleRG) * rippleRingStrength);
    color = lerp(color, rippleRingColor * surfaceIrradiance, rippleRing);
    /// @warning ここで色を事前圧縮しないこと。水面は HDR バッファへブレンド描画され、露出・ACES
    /// @note トーンマップは Composite パスが一括で行う。事前圧縮するときらめきが Bloom に乗らない。
    color = max(color, 0.0f);

    /// @note 屈折と濁りはこの色の中で合成済み。ブレンドで背後をもう一度混ぜると、水底が二重に
    /// @note 入って反射ときらめきが薄まるので、アルファは外周フェードだけに使う。
    /// @note 縁ではコピー前のシーンと同じ背後へ溶けるので、屈折を合成済みでも継ぎ目は出ない。
    float alpha = splineSurface ? 1.0f
        : WaterEdgeFade(p.uv, g_refractionFlowParams.zw, g_normalParams.xy, footprint);
    /// @note 水面は自分自身の重なりを解決するため深度を書く。フェードで見えなくなった縁がそのまま
    /// @note 深度を書くと、その裏の半透明やデカールを «見えない板» が遮る。絵に出ない画素は残さない。
    clip(alpha - 0.003f);
    return float4(color, alpha);
}

#endif
