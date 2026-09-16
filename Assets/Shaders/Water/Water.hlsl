/// @file    Water.hlsl
/// @brief   Gerstner 波・さざ波タイル・深度吸収・屈折・空反射・白波を合成する水面シェーダー。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// @note 水面は半透明でシーン深度と HDR カラーを読むため、通常マテリアルとは別のシェーダーに閉じる。
/// @note 法線マップを持たないのは、水面 1 枚ごとにタイリングとスクロール速度を詰め直す必要が
///       生じ、大きさの違う水面を並べた瞬間にさざ波の粒度が揃わなくなるため。さざ波はエンジンが
///       起動時に焼くタイラブル勾配タイルをワールド座標で引き、反射は空連動 IBL から取る。
///       オーサリング資産 0 個で、どの大きさの水面でも同じ細かさになる。
/// @see Docs/design/water-waves.md
#include "Common/Binding.hlsli"

#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS 4
/// @note Water SSR は PS 内で走るため、全画面 Compute SSR より低い上限にして面積負荷を抑える。
#define WATER_SSR_MAX_STEPS 16

struct PointLightData
{
    float3 position;
    float  range;
    float3 color;
    float  intensity;
};

struct SpotLightData
{
    float3 position;
    float  range;
    float3 direction;
    float  innerCos;
    float3 color;
    float  outerCos;
    float  intensity;
    float3 _pad;
};

cbuffer CameraConstants : register(CB_CAMERA)
{
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float3   cameraPos;
    float    nearZ;
    float    farZ;
    float    waterSsrEnabled;
    float    isOrthographic;
    float    _camPad;
};

/// @note Water は半透明 Forward 描画で GBuffer に法線を書かないため、通常の SSR Compute の
///       反射元にはなれない。共通設定だけを受け取り、水面 PS 内でコピー済み深度を追跡する。
/// @warning 手書きの部分コピーは禁止。以前は先頭 8 フィールドだけを写しており、末尾へ足した
///          画面空間 AO / 接触影が Water からは読めなかった。
#include "Common/AdvancedGraphicsConstants.hlsli"

/// @note C++ の WaterCB と 16 byte 単位で同期する。Vector4 パックにして暗黙パディング差をなくす。
cbuffer WaterCB : register(CB_OBJECT)
{
    float4x4 g_worldMatrix;
    float4x4 g_wvpMatrix;
    float4   g_shallowColorDepth;    ///< xyz=浅瀬色, w=浅瀬深度
    float4   g_deepColorDepth;       ///< xyz=深部色, w=深部深度
    float4   g_surfaceParams;        ///< x=opacity, y=reflectivity, z=fresnelBias, w=fresnelPower
    float4   g_normalParams;         ///< xy=水面のワールド実寸 [m], z=さざ波タイルの勾配復号係数, w=normalStrength
    float4   g_timeParams;           ///< xy=頂点グリッド 1 セルの実寸 [m], z=1/タイルのセル数, w=time
    float4   g_foamParams;           ///< x=threshold, y=fade, z=strength, w=foamNoiseScale
    float4   g_refractionFlowParams; ///< x=refraction, y=flowSpeed, zw=外周フェード幅 (UV 単位, 0 で無効)
    float4   g_waveDir[4];           ///< xy=direction, z=steepness, w=enabled
    float4   g_waveParams[4];        ///< x=amplitude, y=wavelength, z=omega, w=k
    float4   g_detailParams;         ///< x=detailScale, y=detailSpeed, z=detailStrength, w=smoothness
    float4   g_sssParams;            ///< xyz=透過光の色, w=強度
    float4   g_reflectParams;        ///< x=skyReflection(0で無効), y=波の群の深さ, z=水面基準Y, w=波高合計
    float4   g_flowParams;           ///< xy=流れ方向(正規化), z=さざ波の異方比, w=うねり追従ワープ幅 [m]
    float4   g_waveShapeParams;      ///< x=方向広がり [0,1] (0 で 1 波 1 方向), yzw=予約
};

/// @note ユーザー定義エフェクトパラメータ。MaterialComponent.paramData にマップされ、
///       Script から mc->SetParam<float>("rimGlowStrength", val) で動的に書き換えられる。
/// @warning HLSL cbuffer のパッキング規則に従い 16B 境界を揃えること (48 bytes = float4 x3)。
cbuffer MaterialConstants : register(b2)
{
    float  rimGlowStrength;    ///< Row0: リムグロー強度     default 0.40
    float  minShallowAlpha;    ///<       浅瀬の最小アルファ default 0.65
    float  specularStrength;   ///<       スペキュラー強度   default 0.75
    /// @note 旧 specularExponent。ハイライトの鋭さは smoothness と specular AA から導出するため
    ///       廃止した。両方から指数を決めると、遠景でハイライトが 1 ピクセルに縮んで這う。
    float  _pad0;
    float3 skyReflectTint;     ///< Row1: 空反射ベース色 RGB default (0.45, 0.82, 1.0)
    /// @note 旧 envMapBlend。空反射の混合率は skyReflection (g_reflectParams.x) が持つ。
    ///       同じ役目のつまみが 2 つあり、片方だけ動かしても効かない状態だった。
    float  _pad1;
    float3 rippleRingColor;    ///< Row2: 波紋リング色 RGB   default (0.88, 0.97, 1.0)
    float  rippleRingStrength; ///<       波紋リング強度     default 0.72
}

cbuffer LightConstants : register(CB_LIGHT)
{
    float3         lightDir;       float _lightPad;
    float3         lightColor;     float lightIntensity;
    PointLightData pointLights[MAX_POINT_LIGHTS];
    SpotLightData  spotLights[MAX_SPOT_LIGHTS];
    int            pointLightCount;
    int            spotLightCount;
    float2         _lightPad2;
    float3         ambientColor;
    float          _ambientPad;
};

/// @note ShadowConstants (b4) はカスケード配列を含むためレイアウトを 1 か所で定義する。
#include "Common/ShadowConstants.hlsli"

#include "Rendering/Shadow.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Common/BindlessIndices.hlsli"

/// @note 水面が読むテクスチャは «エンジンが生成するもの» だけ。オーサリング資産は要らない。
FBZZ_TEX2D(g_foamMask, 3); ///< 岸沿いの泡マスク (地形高さから CPU 生成)
FBZZ_TEX2D(g_detailNoise, 4); ///< さざ波タイル (起動時に CPU で焼く。RG=勾配, B=高さ)
FBZZ_TEX2D(g_sceneDepth, 5); ///< Water 描画直前の深度コピー
FBZZ_TEX2D(g_sceneColor, 6); ///< Water 描画直前の HDR コピー
FBZZ_TEX2D(g_rippleTex, 8); ///< 着水波紋 (CPU 生成)
FBZZ_TEX2D_T(float, g_shadowMap, 9);
FBZZ_TEXCUBE(g_skyReflection, TEX_IBL_PREFILTER_SLOT); ///< 空連動 IBL の事前フィルタ済みキューブ

/// @note サンプラーのレジスタ割り当ては Binding.hlsli の SAMPLER_* に従う。
/// @warning DX12 は静的サンプラーを Root Signature へ焼き込むため、レジスタごとの意味は全シェーダーで
///          一致していなければならない。Water だけ独自番号を使うと比較サンプラーの位置がずれて影が壊れる。
SamplerState g_sampler      : register(SAMPLER_DEFAULT);
SamplerState g_samplerClamp : register(SAMPLER_LINEAR_CLAMP); ///< 環境反射のサンプルも兼ねる
/// @note さざ波タイルは «寝た視線で引き伸ばされる» のが常態なので異方フィルタを使う。
SamplerState g_samplerTile  : register(SAMPLER_WRAP_ANISO4);
SamplerComparisonState g_shadowSampler : register(SAMPLER_SHADOW);

struct WaterVSInput
{
    float3 position : POSITION;
    float2 uv       : TEXCOORD0;
};

struct WaterPSInput
{
    float4 svPosition : SV_POSITION;
    float3 worldPos   : TEXCOORD0;
    float2 uv         : TEXCOORD1;
    float3 normal     : TEXCOORD2;
    float3 tangent    : TEXCOORD3;
    float3 binormal   : TEXCOORD4;
    float4 screenPos  : TEXCOORD5;
    /// x = 0:谷 1:うねりの山 (透過光の強さ), y = 折り畳み量 + その航跡 (白波の元)
    float2 waveState  : TEXCOORD6;
};

/// @brief 頂点グリッドで «刻めない» 波を寝かせる係数 [0,1]。C++ 側の WaveMeshFade と同じ式。
/// @param cellSize 頂点グリッド 1 セルのワールド実寸 [m]。0 のときはフェードしない。
/// @note Gerstner 波は頂点でしか評価されないので、1 波長あたり数セルしか取れない波は山と谷が
///       セル境界で入れ替わり «もっと長い別の波» に化ける。下限 2.0 は Nyquist、上限 3.5 は
///       «まだ波として読める» 側で、既存の水面から表現できている波を奪わない値。
/// @note 寝かせた波は消えるのではなく WaterResidualWaveSlope が法線だけ拾い直す。
float WaveMeshFade(float wavelength, float2 cellSize)
{
    float cell = max(cellSize.x, cellSize.y);
    if (cell <= 0.0f) return 1.0f;
    return smoothstep(2.0f, 3.5f, wavelength / cell);
}

/// @note 群の向きを波から傾ける角 (0.55 rad) の cos / sin。WaterComponent と同じ値。
static const float kWaveGroupCos = 0.852525f;
static const float kWaveGroupSin = 0.522687f;

/// @brief 波の «群» の包絡 [1-depth, 1+depth]。振幅にそのまま掛ける。
/// @param index 波の番号 [0,3]。群の波長と傾ける向きをここから決める (CB を増やさないため)。
/// @note 正弦を 4 本足しただけの水面はどの波頭も同じ高さ・同じ形になる。実海面は近い周波数
///       どうしの «うなり» で波が群れて進み、大きい波の塊と凪の区間が交互に来る。
/// @note 群の角周波数を r*omega/2 にするのは、深水波の群速度が位相速度の 1/2 だから。
///       これで «群はゆっくり進み、個々の波頭がその中を追い越していく» 見え方になる。
/// @note 群の向きを波から傾けるのは、同じ向きだと波頭が «高さの揃った無限に長い直線» の
///       ままになるため。斜めにずらすと包絡が波頭に沿っても変化し、うねりが塊へ割れる。
/// @warning WaterComponent::WaveGroupEnvelope と同じ式であること。片方だけ変えると、
///          見えている波と浮力が別の水面になる。
float WaveGroupEnvelope(int index, float2 D, float k, float omega, float2 worldXZ, float time)
{
    float depth = saturate(g_reflectParams.y);
    if (depth <= 0.0001f) return 1.0f;

    float ratio = 0.09f + 0.035f * (float)index;
    float sign  = (index & 1) ? -1.0f : 1.0f;
    float2 groupDir = float2(D.x * kWaveGroupCos - D.y * sign * kWaveGroupSin,
                             D.x * sign * kWaveGroupSin + D.y * kWaveGroupCos);
    float phase = (ratio * k) * dot(groupDir, worldXZ) - ratio * omega * 0.5f * time;
    return 1.0f + depth * sin(phase);
}

/// @brief deep-water Gerstner 波を 1 本評価し、同時に解析微分で TBN を更新する。
/// @param index 波の番号 [0,3]。群の包絡に渡す。
/// @param fade  頂点グリッドで刻めない波を寝かせる係数。振幅は更に群の包絡が掛かる。
/// @note CPU 頂点へ法線・接線を持たせず、波変位後の正しい法線を GPU で復元するため。
/// @note 包絡は位置で変わるので厳密な微分には ∂envelope/∂x の項が付くが、包絡の波数は波の
///       1 割ほどしかないため無視する。法線がゆっくり数 % ずれるだけで絵には出ない。
/// @warning 方向が 0 の «使っていない波» で normalize が NaN を返すため、包絡は必ず
///          早期リターンの後で評価すること。NaN は fade <= 0 の判定をすり抜けて全体へ伝播する。
float3 GerstnerDisplace(int index, float4 dirData, float4 params, float3 pos, float time, float fade,
                        inout float3 tangent, inout float3 binormal)
{
    if (dirData.w <= 0.0f || fade <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);

    float2 D = normalize(dirData.xy);
    float  Q = saturate(dirData.z);
    float  A = params.x * fade
             * WaveGroupEnvelope(index, D, params.w, params.z, pos.xz, time);
    float  k = params.w;
    float  omega = params.z;
    float  phi = k * dot(D, pos.xz) - omega * time;
    float  s = sin(phi);
    float  c = cos(phi);

    tangent.x  -= Q * D.x * D.x * k * A * s;
    tangent.y  += D.x * k * A * c;
    tangent.z  -= Q * D.x * D.y * k * A * s;
    binormal.x -= Q * D.x * D.y * k * A * s;
    binormal.y += D.y * k * A * c;
    binormal.z -= Q * D.y * D.y * k * A * s;

    return float3(Q * A * D.x * c, A * s, Q * A * D.y * c);
}

/// @brief 方向広がりの成分ごとの «向きと振幅倍率»。
///
/// @param component 0 = 主成分, 1 = 伴走成分。
/// @param rotation  伴走成分を回す量。x=cos, y=sin。呼び出し側でループの外に出しておく。
/// @note 実海面の波は 1 方向へ揃わず狭い方向スペクトルを持つ。同じ波数で向きだけ違う波を重ねると、
///       波頭が有限の長さに切れる (short-crested sea)。«無限に長い直線の波頭» が消える。
/// @note 振幅は main^2 + comp^2 = 1 で分ける。分けないと spread を上げるだけで海が高くなる。
/// @warning WaterComponent::ApplyWaveSpread と同じ式であること。片方だけ変えると、
///          見えている波と浮力が別の水面になる。
void ApplyWaveSpread(int index, int component, float2 rotation, float2 scales,
                     inout float2 direction, out float amplitudeScale)
{
    if (component == 0) { amplitudeScale = scales.x; return; }
    amplitudeScale = scales.y;
    float sign = (index & 1) ? -1.0f : 1.0f;
    direction = float2(direction.x * rotation.x - direction.y * sign * rotation.y,
                       direction.x * sign * rotation.y + direction.y * rotation.x);
}

/// @brief 方向広がりの «回す量 (cos, sin)» と «主成分 / 伴走成分の振幅倍率» を一度だけ求める。
/// @note 波ごとに三角関数を回さないためにループの外へ括り出す。回す向きは波の番号で交互に
///       入れ替わるので、回転そのものは 2 種類しかない。
void ResolveWaveSpread(out float2 rotation, out float2 scales)
{
    float spread = saturate(g_waveShapeParams.x);
    float energy = saturate(0.40f * spread);
    scales = float2(sqrt(1.0f - energy), sqrt(energy));
    float angle = spread * 0.9f;
    rotation = float2(cos(angle), sin(angle));
}

/// @brief 水平ヤコビアン det(∂(x+d)/∂x) を «指定した時刻の» 波から求める。
/// @param basePos 変位«前»のワールド位置。Gerstner の位相はここで取る。
/// @note 2x2 は sin だけで決まる (cos の項は縦方向にしか効かない)。過去を引くのに変位も法線も
///       要らないので、1 波あたり sin 2 回 (位相と群の包絡) で済む。
float WaveFoldAt(float3 basePos, float time)
{
    float txx = 1.0f, txz = 0.0f, bzx = 0.0f, bzz = 1.0f;
    float2 rotation, scales;
    ResolveWaveSpread(rotation, scales);

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        if (g_waveDir[i].w <= 0.0f) continue;
        float fade = WaveMeshFade(g_waveParams[i].y, g_timeParams.xy);
        if (fade <= 0.0f) continue;

        [unroll]
        for (int part = 0; part < 2; ++part)
        {
            float2 D = normalize(g_waveDir[i].xy);
            float  scale;
            ApplyWaveSpread(i, part, rotation, scales, D, scale);
            if (scale <= 0.001f) continue;

            float k = g_waveParams[i].w;
            float A = g_waveParams[i].x * fade * scale
                    * WaveGroupEnvelope(i, D, k, g_waveParams[i].z, basePos.xz, time);
            float qkAs = saturate(g_waveDir[i].z) * k * A
                       * sin(k * dot(D, basePos.xz) - g_waveParams[i].z * time);
            txx -= D.x * D.x * qkAs;
            txz -= D.x * D.y * qkAs;
            bzx -= D.x * D.y * qkAs;
            bzz -= D.y * D.y * qkAs;
        }
    }
    return txx * bzz - txz * bzx;
}

/// @note 白波は «崩れた跡» が数秒残る。ヤコビアンの瞬間値だけで出すと、波頭が折れた一瞬だけ
///       白くなって消える ── 泡が点滅して «海が生きていない» 見え方になる。
/// @note 同じ «水の粒» の少し前の折り畳みを減衰させながら重ねると、波頭が通り過ぎた後ろへ泡が
///       残る。Gerstner の粒はその場で円を描くので、変位前の位置がそのまま粒の識別子になり、
///       泡は «水にくっついて» 残る。履歴バッファも CPU との同期も要らない。
#define WATER_FOAM_TRAIL_TAPS 3
static const float kFoamTrailStep  = 0.55f; ///< 1 タップあたり何秒さかのぼるか
static const float kFoamTrailDecay = 0.62f; ///< 1 タップごとに残る割合

WaterPSInput VSMain(WaterVSInput v)
{
    WaterPSInput o;
    float time = g_timeParams.w;
    float3 worldPos = mul(float4(v.position, 1.0f), g_worldMatrix).xyz;
    float3 tangent = float3(1.0f, 0.0f, 0.0f);
    float3 binormal = float3(0.0f, 0.0f, 1.0f);
    float3 disp = float3(0.0f, 0.0f, 0.0f);

    /// @note 変位前の位置は «水の粒» の識別子。泡の航跡を引くのに使うので取っておく。
    float3 basePos = worldPos;

    float2 rotation, scales;
    ResolveWaveSpread(rotation, scales);

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float fade = WaveMeshFade(g_waveParams[i].y, g_timeParams.xy);
        [unroll]
        for (int part = 0; part < 2; ++part)
        {
            float2 D = g_waveDir[i].w > 0.0f ? normalize(g_waveDir[i].xy) : float2(1.0f, 0.0f);
            float  scale;
            ApplyWaveSpread(i, part, rotation, scales, D, scale);
            if (scale <= 0.001f) continue;

            float4 dirData = float4(D, g_waveDir[i].z, g_waveDir[i].w);
            float4 params  = g_waveParams[i];
            params.x *= scale;
            disp += GerstnerDisplace(i, dirData, params, worldPos, time, fade, tangent, binormal);
        }
    }

    worldPos += disp;

    /// @note 水平方向のヤコビアン。正規化前の tangent/binormal がそのまま ∂(x+d)/∂x の 2x2 になる。
    ///       1 を割るほど «波が前のめりに詰まっている»、負で «折り畳んでいる» = 崩れる波頭。
    ///       白波を高さで出すと山のてっぺんに丸く乗るが、実際の白波は波の前面に立つ。
    float jacobian = tangent.x * binormal.z - tangent.z * binormal.x;
    float fold = saturate(1.0f - jacobian);

    /// @note 少し前の折り畳みを減衰させながら重ね、波頭の後ろへ泡の帯を残す。
    [unroll]
    for (int tap = 1; tap <= WATER_FOAM_TRAIL_TAPS; ++tap)
    {
        float past = saturate(1.0f - WaveFoldAt(basePos, time - kFoamTrailStep * (float)tap));
        fold = max(fold, past * pow(kFoamTrailDecay, (float)tap));
    }

    tangent = normalize(tangent);
    binormal = normalize(binormal);
    /// @note tangent = ∂P/∂x, binormal = ∂P/∂z。法線は cross(binormal, tangent) で +Y を向く。
    /// @warning 逆順 cross(tangent, binormal) は平坦な水面で (0,-1,0) を返す。以前はこれで法線が
    ///          真下を向き、NdotV が常に 0 → フレネル飽和・スペキュラ消失・影の反転を起こしていた。
    float3 normal = normalize(cross(binormal, tangent));

    o.svPosition = mul(float4(worldPos, 1.0f), viewProjection);
    o.worldPos = worldPos;
    o.uv = v.uv;
    o.normal = normal;
    o.tangent = tangent;
    o.binormal = binormal;
    o.screenPos = o.svPosition;
    /// @note 変位そのものから山の高さを取る。親 Transform があっても基準面がずれない。
    o.waveState = float2(saturate(disp.y / max(g_reflectParams.w, 0.01f)), fold);
    return o;
}

/// @brief さざ波タイルから «1 ノイズセル単位の» 勾配と高さを引く。
/// @param q       ノイズセル単位の座標。
/// @param dqdx    q の画面 x 微分。異方フィルタの LOD 選択に使う。
/// @param dqdy    q の画面 y 微分。
/// @return xy = ∂h/∂q, z = 高さ [0,1]。
/// @note SampleGrad を使うのは、呼び出し元のオクターブループが break を持ち、勾配が非一様制御流れの
///       中では未定義になるため。明示勾配なら break を残したまま正しい mip を選べる。
/// @note タイルの縮小は «勾配の平均» なので、遠景では自動的に平坦へ収束し、水面が鏡へ近づく。
float3 WaterNoiseTile(float2 q, float2 dqdx, float2 dqdy)
{
    float invCells = g_timeParams.z;
    float4 t = g_detailNoise.SampleGrad(g_samplerTile, q * invCells,
                                        dqdx * invCells, dqdy * invCells);
    return float3((t.rg * 2.0f - 1.0f) * g_normalParams.z, t.b);
}

/// @note オクターブごとの固有ドリフト。同じ向きに揃うと縞が流れて見えるため方向をばらす。
#define WATER_DETAIL_OCTAVES 5
static const float2 kWaterDrift[WATER_DETAIL_OCTAVES] = {
    float2( 0.31f,  0.17f), float2(-0.23f,  0.41f),
    float2( 0.47f, -0.29f), float2(-0.37f, -0.13f),
    float2( 0.11f,  0.53f)
};
/// @note 振幅 0.55^i (i = 0..4) の総和。途中で打ち切っても正規化がぶれないよう定数で持つ。
static const float kWaterDetailNorm = 2.110381f;

/// @brief さざ波の高さ勾配 (∂h/∂x, ∂h/∂z) をワールド XZ で積む。
/// @param worldXZ   ワールド XZ [m]。UV で評価すると extent の違う水面どうしで粒度が揃わない。
/// @param footprint このピクセルが覆うワールド距離 [m]。遠いほど・浅い角度ほど大きい。
/// @param dWorldDx  worldXZ の画面 x 微分。タイルの異方フィルタへ渡す。
/// @param dWorldDy  worldXZ の画面 y 微分。
/// @return xy = 勾配, z = Nyquist で落とした細部の割合 (0 = 全部残った, 1 = 全部落ちた)。
/// @note 風向に直交して座標を縮め、さざ波の «うね» を進行方向と直交させる。等方ノイズのままだと
///       粒の集まりにしか見えず、水というより «ブツブツした膜» になる。
float3 WaterDetailGradient(float2 worldXZ, float time, float footprint,
                           float2 dWorldDx, float2 dWorldDy)
{
    float scale = max(g_detailParams.x, 1.0e-4f);
    float speed = g_detailParams.y;
    float2 flow = g_flowParams.xy * (speed * time);

    float  stretch = max(g_flowParams.z, 1.0f);
    float2 alongFlow = g_flowParams.xy;
    float2 crossFlow = float2(-alongFlow.y, alongFlow.x);
    float2x2 aniso = float2x2(alongFlow.x, alongFlow.y, crossFlow.x / stretch, crossFlow.y / stretch);

    /// @note オクターブごとに座標を回し、格子が縞として残らないようにする。異方スケールを
    ///       初期値に畳んでおけば、以降は回転を掛け足すだけで両方が同時に効く。
    float2x2 m = aniso;
    const float2x2 step = float2x2(0.80f, -0.60f, 0.60f, 0.80f);

    float2 grad = float2(0.0f, 0.0f);
    float amp = 1.0f, freq = scale, kept = 0.0f;

    [loop]
    for (int i = 0; i < WATER_DETAIL_OCTAVES; ++i)
    {
        /// @note 1 ピクセルに 1 周期以上入るオクターブは、平均すれば «ざらつき» しか残らない。
        ///       残すとカメラが動くたびに遠景の水面が総毛立って明滅する。周期がピクセルの 2 倍を
        ///       切ったところから滑らかに寝かせ、«細部が消えて鏡に近づく» 見え方へ収束させる。
        ///       freq は単調増加なので、ここで潰れた先のオクターブはすべて潰れている。
        float fade = saturate(1.0f - footprint * freq * 2.0f);
        if (fade <= 0.0f) break;

        float2 q = mul(m, worldXZ) * freq + (flow + kWaterDrift[i] * (time * speed)) * freq;
        float3 n = WaterNoiseTile(q, mul(m, dWorldDx) * freq, mul(m, dWorldDy) * freq);
        /// @note 勾配は変換後の座標系で出るので、mul(g, m) = m^T g で元の軸へ戻す (連鎖律)。
        grad += mul(n.xy, m) * (amp * freq * fade);
        kept += amp * fade;
        m     = mul(step, m);
        amp  *= 0.55f;
        freq *= 2.07f;
    }

    const float invNorm = 1.0f / kWaterDetailNorm;
    return float3(grad * invNorm, saturate(1.0f - kept * invNorm));
}

/// @brief 頂点グリッドで刻めなかった Gerstner 波を «法線だけ» 戻す。
/// @param footprint このピクセルが覆うワールド距離 [m]。
/// @param[out] lost ピクセルより細かくて捨てた量 [0,1]。粗さへ回す。
/// @return ワールド XZ の高さ勾配 (∂h/∂x, ∂h/∂z)。
/// @note WaveMeshFade は海サイズの水面で 4 本すべてを寝かせる。変位を諦めるのは正しいが、法線まで
///       消すと «空を映すだけの板» になる。波長がピクセルより十分大きい間は傾きとして返せる。
/// @note 傾きの上限 1.2 (約 50 度) は安全弁。ak > 1 の波は本来砕けており、変位を伴わないここでは
///       そのままの傾きを出すと «壁» に見える。
float2 WaterResidualWaveSlope(float2 worldXZ, float time, float footprint, out float lost)
{
    float2 slope = float2(0.0f, 0.0f);
    lost = 0.0f;

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        if (g_waveDir[i].w <= 0.0f) continue;
        float wavelength = max(g_waveParams[i].y, 1.0e-4f);
        float residual = 1.0f - WaveMeshFade(wavelength, g_timeParams.xy);
        if (residual <= 0.001f) continue;

        float pixelFade = saturate(1.0f - footprint * 2.0f / wavelength);
        float2 D = normalize(g_waveDir[i].xy);
        float  A = g_waveParams[i].x * residual;
        float  phi = g_waveParams[i].w * dot(D, worldXZ) - g_waveParams[i].z * time;
        slope += D * (A * g_waveParams[i].w * cos(phi)) * pixelFade;
        lost  += residual * (1.0f - pixelFade) * 0.25f;
    }

    float len = length(slope);
    if (len > 1.2f) slope *= 1.2f / len;
    lost = saturate(lost);
    return slope;
}

/// @brief 接空間法線。tangent = +X(world), binormal = +Z(world) なので xy がそのまま world XZ に対応する。
/// @param geoNormal 頂点法線 (ワールド)。うねりの斜面を読み取り、さざ波の分布へ反映する。
/// @return xyz = 接空間法線, w = 落とした細部量 (specular AA で粗さへ回す)。
/// @note うねりの勾配でさざ波の座標をずらす (領域ワープ)。波が «さざ波を運ぶ» ので、ワープが無いと
///       うねりとさざ波が別々の層として滑って見える。ワープ量は画面上でゆっくり変わるため、
///       異方フィルタへ渡す微分には含めていない。
float4 SampleWaterNormal(float2 worldXZ, float2 uv, float time, float footprint,
                         float2 dWorldDx, float2 dWorldDy, float3 geoNormal)
{
    float2 swellSlope = -geoNormal.xz / max(geoNormal.y, 0.2f);
    float2 warped = worldXZ + swellSlope * g_flowParams.w;

    float3 detail = WaterDetailGradient(warped, time, footprint, dWorldDx, dWorldDy);
    /// @note 風上を向いた斜面ほどさざ波が立ち、風下は凪ぐ。一様に散らすとうねりが «ただの起伏» に見える。
    float windward = saturate(0.5f + dot(swellSlope, g_flowParams.xy));
    float2 grad = detail.xy * max(g_detailParams.z, 0.0f) * lerp(0.55f, 1.35f, windward);

    float residualLost = 0.0f;
    grad += WaterResidualWaveSlope(worldXZ, time, footprint, residualLost);

    float3 waveNormal = normalize(float3(-grad.x, -grad.y, 1.0f));

    float2 ripple = g_rippleTex.Sample(g_samplerClamp, uv).rg * 2.0f - 1.0f;
    float3 rippleNormal = float3(ripple.xy, sqrt(saturate(1.0f - dot(ripple.xy, ripple.xy))));
    waveNormal = normalize(waveNormal + rippleNormal * 0.5f);
    return float4(normalize(lerp(float3(0.0f, 0.0f, 1.0f), waveNormal, g_normalParams.w)),
                  saturate(max(detail.z, residualLost)));
}

/// @brief 水面メッシュ外周のフェード係数。矩形の縁でアルファを落とす。
/// @param widthUV   CPU が «メートル / extent» で渡す帯幅 (UV 単位)。0 でフェード無効。
/// @param extent    水面のワールド実寸 [m]。帯をピクセル幅で下支えするのに使う。
/// @param footprint 1 ピクセルが覆うワールド距離 [m]。
/// @note 水面はゼロ厚のシートなので、矩形の縁では «板の切り口» のような直線でシーンが切り替わる。
///       縁へ向かって消していけば «岸に向かって薄くなる水» として読める。
/// @note 帯の幅はメートル固定なので、海サイズでは縁が遠すぎて 1 ピクセルに収まる。画面上で数ピクセル
///       ぶんを下支えすれば、どんな大きさでも縁は溶けたまま消える。上限 0.05 / 0.15 は、寝た視線で
///       遠景の水がまとめて消えるのと、小さな水面の中央まで薄まるのを防ぐ歯止め。
float WaterEdgeFade(float2 uv, float2 widthUV, float2 extent, float footprint)
{
    float2 minWidth = min(footprint * 4.0f / max(extent, 1.0e-4f), 0.05f);
    float2 w = min(max(widthUV, minWidth), 0.15f);
    float2 d = min(uv, 1.0f - uv); ///< 最も近い縁までの距離 [0, 0.5]
    float fx = widthUV.x > 1.0e-5f ? smoothstep(0.0f, w.x, d.x) : 1.0f;
    float fy = widthUV.y > 1.0e-5f ? smoothstep(0.0f, w.y, d.y) : 1.0f;
    return fx * fy;
}

/// @brief 深度バッファの生値を視空間 Z へ直す。
/// @note 平行投影では深度が既に線形。透視用の逆数式を通すと水深フェード・屈折・水面 SSR が一斉にずれる。
float LinearizeDepth(float rawDepth)
{
    if (isOrthographic > 0.5f) return nearZ + rawDepth * (farZ - nearZ);
    return (nearZ * farZ) / max(farZ - rawDepth * (farZ - nearZ), 0.0001f);
}

/// @brief 水面専用 SSR。
/// @return rgb = 反射色, a = 信頼度 (0 でヒット無し)。
/// @note Water 描画直前の sceneDepth / sceneColor を使うため、現在の水面を読み戻す競合を起こさず、
///       既に描画済みの不透明・半透明オブジェクトを反射できる。
float4 TraceWaterSSR(float3 worldPos, float3 normal)
{
    if (waterSsrEnabled < 0.5f || ssrIntensity <= 0.0f || ssrSteps <= 0)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    float3 incident = normalize(worldPos - cameraPos);
    float3 rayDirVS = normalize(mul(float4(reflect(incident, normal), 0.0f), view).xyz);
    float3 rayPosVS = mul(float4(worldPos, 1.0f), view).xyz + rayDirVS * max(ssrThickness, 0.02f);
    /// @note RenderSettings の SSR 品質をそのまま流すと、広い水面で step 数×ピクセル数の負荷が跳ねる。
    int waterSsrSteps = min(ssrSteps, WATER_SSR_MAX_STEPS);
    float stepLength = ssrMaxDistance / max((float)waterSsrSteps, 1.0f);

    [loop]
    for (int step = 0; step < waterSsrSteps; ++step)
    {
        rayPosVS += rayDirVS * stepLength;
        if (rayPosVS.z <= nearZ || rayPosVS.z >= farZ)
            break;

        float4 clip = mul(float4(rayPosVS, 1.0f), projection);
        if (clip.w <= 0.0f)
            break;

        float2 rayUV = clip.xy / clip.w * float2(0.5f, -0.5f) + 0.5f;
        if (any(rayUV <= 0.0f) || any(rayUV >= 1.0f))
            break;

        float sceneRawDepth = g_sceneDepth.SampleLevel(g_samplerClamp, rayUV, 0).r;
        if (sceneRawDepth >= 0.9999f)
            continue;

        float sceneViewDepth = LinearizeDepth(sceneRawDepth);
        float depthDelta = rayPosVS.z - sceneViewDepth;
        float hitThickness = max(ssrThickness, stepLength * abs(rayDirVS.z));
        if (depthDelta >= 0.0f && depthDelta <= hitThickness)
        {
            /// @note 画面端では不安定なヒットを環境反射へ滑らかにフォールバックする。
            float2 edgeDistance = min(rayUV, 1.0f - rayUV);
            float confidence = saturate(min(edgeDistance.x, edgeDistance.y) * 12.0f);
            return float4(g_sceneColor.SampleLevel(g_samplerClamp, rayUV, 0).rgb,
                          confidence * saturate(ssrIntensity));
        }
    }
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
}

float4 PSMain(WaterPSInput p) : SV_Target0
{
    float time = g_timeParams.w;
    float2 screenUV = p.screenPos.xy / p.screenPos.w * float2(0.5f, -0.5f) + 0.5f;

    /// @note 1 ピクセルが覆うワールド距離。さざ波の LOD・タイルの異方フィルタ・specular AA が
    ///       すべてこれを基準にする。
    float2 footprintDX = ddx(p.worldPos.xz);
    float2 footprintDY = ddy(p.worldPos.xz);
    float  footprint   = max(length(footprintDX), length(footprintDY));

    float4 normalSample  = SampleWaterNormal(p.worldPos.xz, p.uv, time, footprint,
                                             footprintDX, footprintDY, p.normal);
    float3 tangentNormal = normalSample.xyz;
    float  lostDetail    = normalSample.w;

    float3x3 tbn = float3x3(normalize(p.tangent), normalize(p.binormal), normalize(p.normal));
    float3 N = normalize(mul(tangentNormal, tbn));
    float3 V = normalize(cameraPos - p.worldPos);
    /// @note 水面下から見上げているときは面の裏側を見ている (PSO は両面描画)。
    /// @warning 向きの判定に N を使うと、浅い角度のピクセルだけがばらばらに反転して斑になる。
    ///          決定は滑らかな幾何法線に任せる。
    if (dot(p.normal, V) < 0.0f) N = -N;
    float NdotV = saturate(dot(N, V));

    /// @note Schlick フレネル。g_surfaceParams.z を水の F0 (実測 0.02 前後) として扱う。
    /// @warning 以前の bias + (1-bias)*pow は grazing 角以外でも下駄を履かせており、真上から見た
    ///          水面まで一定量の反射が乗って «板に空が映っている» 見え方になっていた。
    float f0 = saturate(g_surfaceParams.z);
    float fresnel = f0 + (1.0f - f0) * pow(saturate(1.0f - NdotV), max(g_surfaceParams.w, 1.0f));
    fresnel = saturate(fresnel * g_surfaceParams.y * 2.0f);

    float rawSceneDepth = g_sceneDepth.Sample(g_samplerClamp, screenUV).r;
    /// @note 深度が far plane に張り付く場所は Terrain / Mesh が無い背景ピクセルとして扱う。
    ///       背景の skydome 色を屈折色として読むと、水面が空そのものに溶けてしまうため、
    ///       «底が見えない深い水» として描く。
    float backgroundMask = step(0.9999f, rawSceneDepth);
    float linearSceneDepth = LinearizeDepth(rawSceneDepth);
    float linearSurfDepth = max(p.screenPos.w, 0.0001f);
    float waterDepth = lerp(max(0.0f, linearSceneDepth - linearSurfDepth), g_deepColorDepth.w, backgroundMask);
    float depthFactor = smoothstep(0.0f, 1.0f, saturate(waterDepth / max(g_deepColorDepth.w, 0.0001f)));

    float shallowFactor = smoothstep(0.0f, 1.0f, saturate(waterDepth / max(g_shallowColorDepth.w, 0.0001f)));
    /// @note 色だけは «うねりの山ほど水の層が薄い» ことを織り込む。水底までの距離だけで決めると
    ///       波の山も谷も同じ色になり、うねりが «色の付いた板» にしか見えない。
    ///       アルファと屈折の判定には素の depthFactor を使う (そちらは «底までの距離» の話)。
    float colorDepthFactor = saturate(depthFactor * (1.0f - p.waveState.x * 0.40f));
    float3 waterColor = lerp(g_shallowColorDepth.xyz, g_deepColorDepth.xyz, colorDepthFactor);
    float3 absorptionTint = lerp(float3(1.0f, 1.0f, 1.0f), g_deepColorDepth.xyz, saturate(colorDepthFactor * 0.45f));

    /// @note スクリーンスペース屈折は水面法線で HDR カラー参照 UV をずらす。水底ジオメトリを
    ///       再描画せず透明水面らしい歪みを得る。現在描画中の HDR RT を直接読むと read/write
    ///       競合になるため、Water 直前にコピーした sceneColor を参照する。
    float refractionMask = shallowFactor * (1.0f - backgroundMask);
    float2 refrOffset = tangentNormal.xy * g_refractionFlowParams.x * (1.0f - saturate(fresnel)) * refractionMask;
    float2 refrUV = saturate(screenUV + refrOffset);

    /// @note 屈折先が水面より手前 (= カメラと水面の間に物体がある) なら屈折させない。
    ///       そのまま UV をずらすと手前オブジェクトのシルエットが水中に滲み出す。
    float refrRawDepth    = g_sceneDepth.Sample(g_samplerClamp, refrUV).r;
    float refrLinearDepth = LinearizeDepth(refrRawDepth);
    if (refrLinearDepth < linearSurfDepth)
        refrUV = screenUV;
    float3 refractColor = g_sceneColor.Sample(g_samplerClamp, refrUV).rgb;
    /// @note 背景 (底が見えない) ピクセルだけ水色そのものへ倒す。
    /// @warning 以前は refractColor の輝度がゼロに近いほど waterColor へ寄せていたため、水中の
    ///          暗い岩や影が水色に塗り潰されて沈んだ物体が見えなくなっていた。
    float3 baseRefract = lerp(refractColor * absorptionTint, waterColor, backgroundMask);
    waterColor = lerp(baseRefract, waterColor, saturate(depthFactor * 0.65f));

    /// @note 空反射は空連動 IBL の事前フィルタ済みキューブから引く (専用の環境テクスチャは不要)。
    /// @warning 反射ベクトルを水平線より下へ向けないこと。遠い水面ほど視線が寝て R は水平線すれすれを
    ///          向き、さざ波が 1 つ揺れるだけで R が下半球 (ほぼ真っ黒) へ落ちて黒い帯が出る。
    float3 R = reflect(-V, N);
    R = normalize(float3(R.x, max(R.y, 0.02f), R.z));
    /// @note 落とした細部はサブピクセルの法線ばらつきそのものなので粗さへ移す (specular AA)。
    ///       移さないと «消えた細部» が反射とハイライトからだけ抜け落ち、遠景の水面が磨いた金属板になる。
    float  roughness = saturate(1.0f - g_detailParams.w);
    float  roughnessAA = saturate(roughness + lostDetail * (1.0f - roughness) * 0.70f);
    /// @note 寝た視線では粗さを引き戻して環境キューブを鮮明な mip から引く。粗い mip は上下 90 度ぶんを
    ///       平均した色で、水平線際では空に地面が混ざって沈む。実際の水面は入射が浅いほど反射ローブが
    ///       細くなるのでこちらが正しい。ハイライト側は 1 ピクセルのちらつきを避けるため据え置く。
    float  horizonSharpen = lerp(0.35f, 1.0f, NdotV);
    float3 reflectColor = skyReflectTint;
    /// @note 透過光の «空側» に使う、水が上から受けている光。IBL が無いときは .mat の空色へ落ちる。
    float3 skyAmbient = skyReflectTint;
    [branch]
    if (g_reflectParams.x > 0.001f)
    {
        float3 sky = g_skyReflection.SampleLevel(g_samplerClamp, R,
                                                 roughnessAA * horizonSharpen
                                                     * (float)max(iblMaxMipLevel, 0)).rgb;
        reflectColor = lerp(skyReflectTint, sky, saturate(g_reflectParams.x));
        /// @note 一番粗いミップを真上から引くと «空を広く平均した色» になる。LightConstants の
        ///       ambientColor は Lit で 0.08 固定 / Unlit で白なので、時刻に追随しない。
        skyAmbient = g_skyReflection.SampleLevel(g_samplerClamp, float3(0.0f, 1.0f, 0.0f),
                                                 (float)max(iblMaxMipLevel, 0)).rgb;
    }

    /// @note 反射ウェイト (フレネル) を先に求め、SSR は寄与が実際に見えるピクセルだけトレースする。
    ///       TraceWaterSSR は WaterForward の主コストで、水面を見下ろす (低フレネル) ピクセルは
    ///       レイマーチを丸ごと省いても結果はほぼ不変。背景ピクセルは画面内にヒット候補が無く
    ///       長い空走査になるため環境反射へフォールバックする。
    /// @warning 背景ピクセルで反射を減らさないこと。そこはフレネルが 1 に張り付き本来は鏡になる
    ///          水平線際で、以前の 0.45 倍で遠い水面が黒く沈んでいた。
    float reflectionWeight = saturate(fresnel);
    [branch]
    if (reflectionWeight > 0.04f && backgroundMask < 0.5f)
    {
        float4 ssrReflection = TraceWaterSSR(p.worldPos, N);
        reflectColor = lerp(reflectColor, ssrReflection.rgb, ssrReflection.a);
    }
    float3 color = lerp(waterColor, reflectColor, reflectionWeight);

    float3 L = normalize(-lightDir);
    /// @note Water は半透明なので影を強く乗算せず、«太陽光の弱まり» として扱う。完全な黒影にすると
    ///       水面下の屈折色まで不自然に消える。
    float shadow = ComputeShadow(g_shadowMap, g_shadowSampler, p.worldPos,
        lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    /// @note Forward の画面空間 AO / 接触影。Deferred では b8 が 0 なので素通りする。
    shadow *= FBZZ_ScreenContactShadow(p.svPosition.xy);
    color *= lerp(0.72f, 1.0f, shadow);

    /// @note 太陽のきらめきは GGX で引く。Blinn-Phong は裾が急に落ちるため、海のきらめきの
    ///       «広がった尾» が出ず、ハイライトが «板に当たった光» に見える。
    /// @note 異方にするのは、さざ波が detailAnisotropy で風向と直交して伸びているから。同じ
    ///       異方を反射ローブへ渡すと、きらめきが風向に沿った帯になる ── 海面の見え方そのもの。
    /// @note 接空間は «風向» で張り直す。p.tangent はワールド +X 基準なので、そのまま渡すと
    ///       異方の向きが風と無関係になる。うねの筋を横切る «風向» 側が粗い。
    float3 flowWorld = float3(g_flowParams.x, 0.0f, g_flowParams.y);
    float3 specT = flowWorld - N * dot(N, flowWorld);
    float  specTLen = length(specT);
    specT = specTLen > 1.0e-4f ? specT / specTLen : normalize(p.tangent);
    float3 specB = normalize(cross(N, specT));
    float  specAniso = saturate((max(g_flowParams.z, 1.0f) - 1.0f) * 0.35f);

    /// @note 太陽は点ではなく角半径 0.0047 rad の円盤。α に円盤ぶんを足して峰を広げないと、
    ///       GGX の頂点が 1 ピクセルへ落ちて Bloom がちらつく (球光源の正規化と同じ扱い)。
    /// @note 峰を広げたぶんだけ (α/α')^2 で落とす。これを掛けないと «広げた» のではなく
    ///       «明るくした» ことになり、光のエネルギーが増える。
    float sunAlpha = max(roughnessAA * roughnessAA, 1.0e-5f);
    float sunAlphaWide = saturate(sunAlpha + 0.0023f);
    float sunEnergy = (sunAlpha / sunAlphaWide) * (sunAlpha / sunAlphaWide);
    float NdotL = saturate(dot(N, L));
    float3 specular = BRDF_SpecularAdvanced(N, V, L, f0.xxx, sqrt(sunAlphaWide),
                                            specT, specB, specAniso) * sunEnergy;
    /// @note 影は «一度掛けてから» 柔らげる。lerp だけにすると、影の中の水面にも太陽の
    ///       きらめきが残る。
    color += lightColor * specular * NdotL * max(lightIntensity, 0.0f)
           * specularStrength * shadow * lerp(0.65f, 1.0f, shadow);

    /// @note 波の背面から透ける光 (subsurface)。«光を通す液体» に見えるかはここで決まり、
    ///       反射とスペキュラだけだと金属板のような水面になる。
    /// @note 光を法線で «曲げて» から視線と比べる (fast subsurface scattering)。素の dot(V,-L)
    ///       だと太陽をちょうど背にした一瞬しか光らず、それ以外の角度で波の内側が常に暗い。
    float3 scatterDir = normalize(L + N * 0.30f);
    float forwardScatter = pow(saturate(dot(V, -scatterDir)), 3.0f);
    /// @note うねりの山ほど水の層が薄く、光が通り抜ける。
    float thickness = p.waveState.x * saturate(g_sssParams.w);
    color += g_sssParams.rgb * lightColor * (forwardScatter * thickness) * lerp(0.4f, 1.0f, shadow);
    /// @note 空からの散乱。太陽が無くても波の内側は明るい。これが無いと曇りや夜の海だけが
    ///       «光を通さない板» に戻る。影の影響を受けないのは、空の光は影の中にも届くから。
    color += g_sssParams.rgb * skyAmbient * (thickness * 0.5f);

    /// @note Forward+ / Deferred+ の局所光。水面は透明材質なので局所光の影は共通影と分離し、
    ///       水色の拡散寄与だけを加算する。Deferred 経路でも Water は HDR へ直接描くためここで評価する。
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        color += Lighting_Lambert_Direct(N, ps.L, waterColor,
            ps.color, ps.intensity);
    FBZZ_PUNCTUAL_END

    float rim = pow(1.0f - NdotV, 3.0f) * rimGlowStrength;
    color += lerp(waterColor, skyReflectTint, 0.35f) * rim;

    /// @note 波打ち際 (地形と水面が交差する浅瀬) に発生する接岸泡。waterDepth が threshold より
    ///       浅いほど強くし、岸辺に沿った白い帯を作る。
    float foamThreshold = g_foamParams.x;
    float foamFade      = max(g_foamParams.y, 0.0001f);
    float shoreFoam     = (1.0f - smoothstep(foamThreshold, foamThreshold + foamFade, waterDepth)) * (1.0f - backgroundMask);

    float foamMaskVal = g_foamMask.Sample(g_samplerClamp, p.uv).r;
    /// @note 泡のムラもさざ波タイルから引く。ワールド座標なので水面の大きさに依らず粒が揃う。
    float foamScale = max(g_foamParams.w, 1.0e-4f);
    float2 foamP = p.worldPos.xz * foamScale + g_flowParams.xy * (time * 0.35f);
    float foamTexVal = saturate(WaterNoiseTile(foamP, footprintDX * foamScale,
                                               footprintDY * foamScale).z * 1.6f);
    /// @note 崩れる白波は «Gerstner 変位が折り畳む» ところに出る。岸が無い外洋でも波が立って
    ///       見えるかはここで決まる。山の高さで出すと白が波頭に丸く乗り、波が «進んで» 見えない。
    float crestFoam = smoothstep(0.55f, 1.0f, p.waveState.y) * 0.85f;
    float foamAmount = max(max(foamMaskVal, shoreFoam), crestFoam) * foamTexVal;
    float foam = smoothstep(0.05f, 1.0f, foamAmount) * g_foamParams.z * lerp(0.80f, 1.0f, shadow);
    float3 foamColor = lerp(float3(0.72f, 0.88f, 0.92f), float3(1.0f, 1.0f, 1.0f), saturate(foamTexVal));
    color = lerp(color, foamColor, saturate(foam));

    float2 rippleRG = g_rippleTex.Sample(g_samplerClamp, p.uv).rg * 2.0f - 1.0f;
    float rippleRing = saturate(length(rippleRG) * rippleRingStrength);
    color = lerp(color, rippleRingColor, rippleRing);
    /// @warning ここで色を事前圧縮しないこと。水面は HDR バッファへブレンド描画され、露出・ACES
    ///          トーンマップは Composite パスが一括で行う。事前圧縮するときらめきが Bloom に乗らない。
    color = max(color, 0.0f);

    float alpha = g_surfaceParams.x * lerp(minShallowAlpha, 1.0f, depthFactor);
    alpha = max(alpha, backgroundMask * 0.92f);
    alpha = saturate(max(alpha, max(foam * 0.9f, rippleRing * 0.95f)));

    /// @note 外周フェードは «最後に» 掛ける。上の max 群より前だと、泡や背景マスクが縁のアルファを
    ///       持ち上げ直してしまい、切り口の線がそのまま残る。
    alpha *= WaterEdgeFade(p.uv, g_refractionFlowParams.zw, g_normalParams.xy, footprint);
    /// @note 水面は自分自身の重なりを解決するため深度を書く。フェードで見えなくなった縁がそのまま
    ///       深度を書くと、その裏の半透明やデカールを «見えない板» が遮る。絵に出ない画素は残さない。
    clip(alpha - 0.003f);
    return float4(color, alpha);
}
