/// @file    Water.hlsl
/// @brief   Gerstner 波・さざ波タイル・深度吸収・屈折・空反射・白波を合成する水面シェーダー。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// @note 水面は半透明でシーン深度と HDR カラーを読むため、通常マテリアルとは別のシェーダーに閉じる。
/// @note 法線マップを持たないのは、水面 1 枚ごとにタイリングとスクロール速度を詰め直す必要が
/// @note 生じ、大きさの違う水面を並べた瞬間にさざ波の粒度が揃わなくなるため。さざ波はエンジンが
/// @note 起動時に焼くタイラブル勾配タイルをワールド座標で引き、反射は空連動 IBL から取る。
/// @note オーサリング資産 0 個で、どの大きさの水面でも同じ細かさになる。
/// @see Docs/design/water-waves.md
#include "Common/Binding.hlsli"

#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS 4
/// @note Water SSR は PS 内で走るため、全画面 Compute SSR より低い上限にして面積負荷を抑える。
#define WATER_SSR_MAX_STEPS 16

/// @note 水面 1 枚が受け取る流れの枠数。C++ の kWaterSurfaceFlowCount と揃えること。
#define WATER_SURFACE_FLOW_COUNT 8
/// @note 流れの型。FlowFieldType と ParticleGpuSim.cs.hlsl の FF_* と同じ整数であること。
#define WFF_UNIFORM 0
#define WFF_SINK    1
#define WFF_SOURCE  2
#define WFF_VORTEX  3
#define WFF_CURL    4
#define WFF_BAKED   6
/// @note 速度場アトラスの 1 タイルの 1 辺。VectorFieldAsset.hpp の
/// @note kVelocityFieldTileResolution と一致させること。
#define WATER_VELOCITY_FIELD_TILE 32

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
    float    waterWireframeMode; ///< @brief 0=通常、1=Wireframe Lit、2=Wireframe Unlit
};

/// @note Water は半透明 Forward 描画で GBuffer に法線を書かないため、通常の SSR Compute の
/// @note 反射元にはなれない。共通設定だけを受け取り、水面 PS 内でコピー済み深度を追跡する。
/// @warning 手書きの部分コピーは禁止。以前は先頭 8 フィールドだけを写しており、末尾へ足した
/// @note 画面空間 AO / 接触影が Water からは読めなかった。
#include "Common/AdvancedGraphicsConstants.hlsli"

/// @note C++ の WaterCB と 16 byte 単位で同期する。Vector4 パックにして暗黙パディング差をなくす。
cbuffer WaterCB : register(CB_OBJECT)
{
    float4x4 g_worldMatrix;
    float4x4 g_wvpMatrix;
    float4   g_shallowColorDepth;    ///< @brief xyz=浅瀬色, w=浅瀬深度
    float4   g_deepColorDepth;       ///< @brief xyz=深部色, w=深部深度
    float4   g_surfaceParams;        ///< @brief x=opacity, y=reflectivity, z=fresnelBias, w=fresnelPower
    float4   g_normalParams;         ///< @brief xy=水面のワールド実寸 [m], z=さざ波タイルの勾配復号係数, w=normalStrength
    float4   g_timeParams;           ///< @brief xy=頂点グリッド 1 セルの実寸 [m], z=1/タイルのセル数, w=time
    float4   g_foamParams;           ///< @brief x=threshold, y=fade, z=strength, w=foamNoiseScale
    float4   g_refractionFlowParams; ///< @brief x=refraction, y=flowSpeed, zw=外周フェード幅 (UV 単位, 0 で無効)
    float4   g_waveDir[4];           ///< @brief xy=direction, z=steepness, w=enabled
    float4   g_waveParams[4];        ///< @brief x=amplitude, y=wavelength, z=omega, w=k
    float4   g_detailParams;         ///< @brief x=detailScale, y=detailSpeed, z=detailStrength, w=smoothness
    float4   g_sssParams;            ///< @brief xyz=透過光の色, w=強度
    float4   g_reflectParams;        ///< @brief x=skyReflection(0で無効), y=波の群の深さ, z=水面基準Y, w=波高合計
    float4   g_flowParams;           ///< @brief xy=流れ方向(正規化), z=さざ波の異方比, w=うねり追従ワープ幅 [m]
    float4   g_waveShapeParams;      ///< @brief x=方向広がり [0,1] (0 で 1 波 1 方向), y=流れの本数, zw=予約
    /// @brief xy=中心のワールド XZ [m], z=影響半径 [m], w=形の変位 [m] (穴は負・山は正)
    float4   g_surfaceFlowA[WATER_SURFACE_FLOW_COUNT];
    /// @brief x=流速 [m/s] (渦だけ回る向きの符号つき), y=減衰の指数, z=型 (WFF_*), w=質感へ回す強さ [0,1]
    float4   g_surfaceFlowB[WATER_SURFACE_FLOW_COUNT];
    /// @brief xy=Uniform の XZ 向き (正規化), z=速度場アトラスのタイル番号 (-1 で無効), w=復号係数
    float4   g_surfaceFlowC[WATER_SURFACE_FLOW_COUNT];
    /// @brief Baked の逆回転クォータニオン (x, y, z, w)
    float4   g_surfaceFlowD[WATER_SURFACE_FLOW_COUNT];
    /// @brief xyz=Baked の箱の半径 [m], w=水面の基準面 Y − 場の中心 Y [m]
    float4   g_surfaceFlowE[WATER_SURFACE_FLOW_COUNT];
};

/// @note 形の Gaussian の実効半径 / 影響半径。C++ の kWaterSurfaceFlowShapeRatio と同じ値。
static const float kWaterFlowShapeRatio = 0.4f;
/// @note 渦と吸い込みの «目» とみなす半径の割合。ここに泡を足す。
static const float kWaterFlowEyeRatio = 0.3f;
/// @note この流速 [m/s] でさざ波の向きと質感が «一様な流れ» と 1:1 で釣り合う。
/// @note 流速そのものではなく «重み» なので、単位を持つ .mat のつまみにはしない。
/// @warning C++ の kWaterSurfaceFlowReference と同じ値であること。
static const float kWaterFlowReference = 4.0f;
/// @note Uniform の «風の足跡» と Curl の «ざわつき» がさざ波の強度を持ち上げる上限。
static const float kWaterWindDetailGain = 0.6f;
static const float kWaterCurlDetailGain = 0.5f;
/// @note 波紋テクスチャの B が表せる高さの範囲 [m] (±)。
/// @warning C++ の kWaterRippleHeightScale と同じ値であること。片方だけ変えると、
/// @note 見えている輪と浮力が別の高さになる。
static const float kRippleHeightScale = 0.5f;

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

cbuffer LightConstants : register(CB_LIGHT)
{
    float3         lightDir;       float _lightPad;
    float3         lightColor;     float lightIntensity;
    PointLightData pointLights[MAX_POINT_LIGHTS];
    SpotLightData  spotLights[MAX_SPOT_LIGHTS];
    int            pointLightCount;
    int            spotLightCount;
    /// @note Common/Constants.hlsli と同じ並び。IBL が無いときの空反射の明るさに使う。
    float          skyDimmer;
    float          _lightPad2;
    float3         ambientColor;
    float          _ambientPad;
};

/// @note ShadowConstants (b4) はカスケード配列を含むためレイアウトを 1 か所で定義する。
#include "Common/ShadowConstants.hlsli"

#include "Rendering/Shadow.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Common/BindlessIndices.hlsli"

/// @note 水面が読むテクスチャは «エンジンが生成するもの» だけ。オーサリング資産は要らない。
FBZZ_TEX2D(g_foamMask, 3); ///< @brief 岸沿いの泡マスク (地形高さから CPU 生成)
FBZZ_TEX2D(g_detailNoise, 4); ///< @brief さざ波タイル (起動時に CPU で焼く。RG=勾配, B=高さ)
FBZZ_TEX2D(g_sceneDepth, 5); ///< @brief Water 描画直前の深度コピー
FBZZ_TEX2D(g_sceneColor, 6); ///< @brief Water 描画直前の HDR コピー
FBZZ_TEX2D(g_rippleTex, 8); ///< @brief 着水波紋 (CPU 生成)
FBZZ_TEX2D_T(float, g_shadowMap, 9);
FBZZ_TEXCUBE(g_skyReflection, TEX_IBL_PREFILTER_SLOT); ///< @brief 空連動 IBL の事前フィルタ済みキューブ
/// @note 常駐中の 速度場 PNG を積んだアトラス。場が 1 枚も無くても 1x1x1 が必ず束縛される。
FBZZ_TEX3D_T(float4, g_velocityAtlas, TEX_VELOCITY_FIELD_SLOT);

/// @note サンプラーのレジスタ割り当ては Binding.hlsli の SAMPLER_* に従う。
/// @warning DX12 は静的サンプラーを Root Signature へ焼き込むため、レジスタごとの意味は全シェーダーで
/// @note 一致していなければならない。Water だけ独自番号を使うと比較サンプラーの位置がずれて影が壊れる。
SamplerState g_sampler      : register(SAMPLER_DEFAULT);
SamplerState g_samplerClamp : register(SAMPLER_LINEAR_CLAMP); ///< @brief 環境反射のサンプルも兼ねる
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
    /// @brief x = 0:谷 1:うねりの山 (透過光の強さ), y = 折り畳み量 + その航跡 (白波の元)
    float2 waveState  : TEXCOORD6;
    /// @brief xy = 流向、z = さざ波強度、w = 泡のムラ。頂点で評価して補間する。
    float4 flowDirectionDetail : TEXCOORD7;
    float2 flowFoam : TEXCOORD8;
};

/// @brief 頂点グリッドで «刻めない» 波を寝かせる係数 [0,1]。C++ 側の WaveMeshFade と同じ式。
/// @param cellSize 頂点グリッド 1 セルのワールド実寸 [m]。0 のときはフェードしない。
/// @note Gerstner 波は頂点でしか評価されないので、1 波長あたり数セルしか取れない波は山と谷が
/// @note セル境界で入れ替わり «もっと長い別の波» に化ける。下限 2.0 は Nyquist、上限 3.5 は
/// @note «まだ波として読める» 側で、既存の水面から表現できている波を奪わない値。
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
/// @note どうしの «うなり» で波が群れて進み、大きい波の塊と凪の区間が交互に来る。
/// @note 群の角周波数を r*omega/2 にするのは、深水波の群速度が位相速度の 1/2 だから。
/// @note これで «群はゆっくり進み、個々の波頭がその中を追い越していく» 見え方になる。
/// @note 群の向きを波から傾けるのは、同じ向きだと波頭が «高さの揃った無限に長い直線» の
/// @note ままになるため。斜めにずらすと包絡が波頭に沿っても変化し、うねりが塊へ割れる。
/// @warning WaterComponent::WaveGroupEnvelope と同じ式であること。片方だけ変えると、
/// @note 見えている波と浮力が別の水面になる。
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
/// @note 1 割ほどしかないため無視する。法線がゆっくり数 % ずれるだけで絵には出ない。
/// @warning 方向が 0 の «使っていない波» で normalize が NaN を返すため、包絡は必ず
/// @note 早期リターンの後で評価すること。NaN は fade <= 0 の判定をすり抜けて全体へ伝播する。
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
/// @note 波頭が有限の長さに切れる (short-crested sea)。«無限に長い直線の波頭» が消える。
/// @note 振幅は main^2 + comp^2 = 1 で分ける。分けないと spread を上げるだけで海が高くなる。
/// @warning WaterComponent::ApplyWaveSpread と同じ式であること。片方だけ変えると、
/// @note 見えている波と浮力が別の水面になる。
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
/// @note 入れ替わるので、回転そのものは 2 種類しかない。
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
/// @note 要らないので、1 波あたり sin 2 回 (位相と群の包絡) で済む。
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
/// @note 白くなって消える ── 泡が点滅して «海が生きていない» 見え方になる。
/// @note 同じ «水の粒» の少し前の折り畳みを減衰させながら重ねると、波頭が通り過ぎた後ろへ泡が
/// @note 残る。Gerstner の粒はその場で円を描くので、変位前の位置がそのまま粒の識別子になり、
/// @note 泡は «水にくっついて» 残る。履歴バッファも CPU との同期も要らない。
#define WATER_FOAM_TRAIL_TAPS 3
static const float kFoamTrailStep  = 0.55f; ///< @brief 1 タップあたり何秒さかのぼるか
static const float kFoamTrailDecay = 0.62f; ///< @brief 1 タップごとに残る割合

/// @brief クォータニオン q でベクトル v を回す。CPU の math::Quaternion::operator* と同じ式。
float3 WaterQuatRotate(float4 q, float3 v)
{
    const float3 t = 2.0f * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

/// @brief 速度場アトラスからタイル 1 枚ぶんを引く。
/// @param local 場のローカル正規化座標 [0,1]³。
/// @note Z を手で補間するのは、タイルが Z 方向に積んであり、ハードウェアのトリリニアだと
/// @note 境界で隣の場が混ざるため。
/// @warning ParticleGpuSim.cs.hlsl の SampleVelocityField と同じ式であること。片方だけ変えると、
/// @note 同じ 速度場 PNG で粒子と水面が別の流れを見る。
float3 WaterSampleVelocityField(uint tile, float3 local, float maxMagnitude)
{
    uint atlasW, atlasH, atlasD;
    g_velocityAtlas.GetDimensions(atlasW, atlasH, atlasD);
    const float tileTexels = (float)WATER_VELOCITY_FIELD_TILE;

    const float z  = local.z * tileTexels - 0.5f;
    const float z0 = clamp(floor(z), 0.0f, tileTexels - 1.0f);
    const float z1 = min(z0 + 1.0f, tileTexels - 1.0f);
    const float t  = saturate(z - z0);

    const float base = (float)tile * tileTexels;
    const float3 a = g_velocityAtlas.SampleLevel(
        g_samplerClamp, float3(local.xy, (base + z0 + 0.5f) / (float)atlasD), 0.0f).rgb;
    const float3 b = g_velocityAtlas.SampleLevel(
        g_samplerClamp, float3(local.xy, (base + z1 + 0.5f) / (float)atlasD), 0.0f).rgb;
    /// @note 復元は CPU の QuantizeVectorFieldValue と同じ式。
    return (lerp(a, b, t) * 2.0f - 1.0f) * maxMagnitude;
}

/// @brief 焼いた場 1 枚のワールド流速 [m/s]。範囲外はゼロ。
/// @param baseXZ **変位前の** ワールド XZ。高さは g_surfaceFlowE[i].w で基準面へ固定する。
/// @note 高さを «波を乗せた後の Y» で引くと «高さを高さで引く» 循環になる。
/// @warning WaterComponent::WaterBakedDisplacement と同じ標本化であること。
float3 WaterBakedVelocity(int i, float2 baseXZ)
{
    if (g_surfaceFlowC[i].z < 0.0f) return float3(0.0f, 0.0f, 0.0f);
    float3 toPoint = float3(baseXZ.x - g_surfaceFlowA[i].x,
                            g_surfaceFlowE[i].w,
                            baseXZ.y - g_surfaceFlowA[i].y);
    float4 invRot = g_surfaceFlowD[i];
    float3 uvw = WaterQuatRotate(invRot, toPoint) / g_surfaceFlowE[i].xyz * 0.5f + 0.5f;
    if (any(uvw < 0.0f) || any(uvw > 1.0f)) return float3(0.0f, 0.0f, 0.0f);

    float3 value = WaterSampleVelocityField((uint)g_surfaceFlowC[i].z, uvw, g_surfaceFlowC[i].w);
    /// @note 場は «場のローカル» で焼かれているのでワールドへ戻す。strength は無次元の倍率。
    return WaterQuatRotate(float4(-invRot.xyz, invRot.w), value) * g_surfaceFlowB[i].x;
}

/// @brief 焼いた場 1 枚が作るへこみ [m] (0 以下)。Bernoulli を XZ 成分だけに掛ける。
/// @warning WaterComponent::WaterBakedDisplacement と同じ式であること。
float WaterBakedDisplacement(int i, float2 baseXZ)
{
    float cap = abs(g_surfaceFlowA[i].w);
    if (cap <= 0.0001f) return 0.0f;
    float3 v = WaterBakedVelocity(i, baseXZ);
    return -min(dot(v.xz, v.xz) / 19.6f, cap);
}

/// @brief 中心を持つ流れ 1 本が作る変位 [m] と、その水平勾配。
/// @param worldXZ **変位後の** ワールド XZ。形は水平変位を持たないので逆写像に入れない。
/// @return x = 変位 [m] (穴は負・山は正), yz = (∂h/∂x, ∂h/∂z)。
/// @note 形を Gaussian にするのは C¹ 連続だから。(1 - r/R)^p は r = R で折れ、頂点法線が
/// @note 1 セルだけ跳ねて縁に輪が出る。
/// @note 勾配は Gerstner 変位を通す連鎖律を無視している (群の包絡と同じ扱い)。形の波数は
/// @note うねりよりずっと小さく、法線が数 % ずれるだけで絵には出ない。
/// @warning WaterComponent::WaterRadialDisplacement と同じ式であること。
float3 WaterRadialDisplacement(int i, float2 worldXZ)
{
    float4 a = g_surfaceFlowA[i];
    if (abs(a.w) <= 0.0001f || a.z <= 0.0001f) return float3(0.0f, 0.0f, 0.0f);
    float  s = a.z * kWaterFlowShapeRatio;
    float2 d = worldXZ - a.xy;
    float  h = a.w * exp(-dot(d, d) / (s * s));
    return float3(h, -2.0f * h * d / (s * s));
}

/// @brief PS が使う «その点の流れと質感»。
struct WaterFlowSample
{
    float2 direction;  ///< @brief さざ波・きらめき・泡が流れる向き (正規化)
    float  eye;        ///< @brief 渦と吸い込みの目への近さ [0,1]。泡へ足す
    float  ring;       ///< @brief 湧き出しの縁への近さ [0,1]。泡の輪
    float  detailGain; ///< @brief さざ波の強度倍率 (1 起点)
    float  chop;       ///< @brief 泡のムラ [0,1]
};

/// @brief その点の流れの向きと、型ごとの質感を集める。
/// @param baseXZ 変位前のワールド XZ (Baked の標本位置)。
/// @note 減衰 pow(saturate(1 - r/radius), falloffPower) は FlowFieldEval の ResolveInfluence と
/// @note 同じ式。中心と同じ高さの点なら CPU の流速と一致する。
/// @note **向きだけを曲げ、さざ波を «運ぶ» 距離は一様なまま。** 速度場で座標を積分すると、
/// @note 流速が場所で変わる渦ではさざ波が輪へ引き伸ばされる (直すには履歴バッファが要る)。
/// @note Curl は向きを曲げない。ピクセルごとにカールノイズを回すことになるうえ、乱流が出したい
/// @note のは «向き» ではなく «ざわつき» なので質感だけに落とす。
WaterFlowSample SampleWaterSurfaceFlow(float2 worldXZ, float2 baseXZ)
{
    WaterFlowSample s;
    s.direction  = g_flowParams.xy;
    s.eye        = 0.0f;
    s.ring       = 0.0f;
    s.detailGain = 1.0f;
    s.chop       = 0.0f;

    float2 dir = g_flowParams.xy;
    int count = (int)g_waveShapeParams.y;

    [loop]
    for (int i = 0; i < WATER_SURFACE_FLOW_COUNT; ++i)
    {
        if (i >= count) continue;
        int kind = (int)g_surfaceFlowB[i].z;

        if (kind == WFF_BAKED)
        {
            float3 v = WaterBakedVelocity(i, baseXZ);
            dir += v.xz / kWaterFlowReference;
            s.detailGain += kWaterWindDetailGain
                          * saturate(length(v.xz) / kWaterFlowReference);
            continue;
        }

        float radius = g_surfaceFlowA[i].z;
        if (radius <= 0.0001f) continue;
        float2 d = worldXZ - g_surfaceFlowA[i].xy;
        float  r = length(d);
        if (r >= radius) continue;

        float influence = pow(saturate(1.0f - r / radius), g_surfaceFlowB[i].y);
        float speed = g_surfaceFlowB[i].x;
        float chop  = g_surfaceFlowB[i].w * influence;

        if (kind == WFF_UNIFORM)
        {
            dir += g_surfaceFlowC[i].xy * (speed * influence / kWaterFlowReference);
            /// @note «風の足跡»。局所の風はうねりを育てないが、さざ波はその場で立つ。
            s.detailGain += kWaterWindDetailGain * chop;
            continue;
        }
        if (kind == WFF_CURL)
        {
            s.detailGain += kWaterCurlDetailGain * chop;
            s.chop = max(s.chop, chop);
            continue;
        }
        if (r <= 1.0e-4f) continue;

        if (kind == WFF_VORTEX)
        {
            /// @note cross(up, d) の XZ 成分。符号は speed が持つ (回転軸の y の符号)。
            dir += float2(d.y, -d.x) / r * (speed * influence / kWaterFlowReference);
            s.eye = max(s.eye, saturate(1.0f - r / (radius * kWaterFlowEyeRatio)));
            continue;
        }
        /// @note Sink は内向き、Source は外向き。
        float sign = (kind == WFF_SOURCE) ? 1.0f : -1.0f;
        dir += d / r * (sign * speed * influence / kWaterFlowReference);
        if (kind == WFF_SINK)
        {
            /// @note 排水口の中心も泡立つ。回る水ほどではないので 0.6 倍。
            s.eye = max(s.eye, saturate(1.0f - r / (radius * kWaterFlowEyeRatio)) * 0.6f);
        }
        else
        {
            /// @note 湧き上がった水は縁で広がってぶつかる。influence は中心がいちばん強いので
            /// @note 輪には使えない。半径の 0.55〜1.0 に帯を立て、強さは流速から取る。
            float t = r / radius;
            s.ring = max(s.ring, smoothstep(0.55f, 0.80f, t) * (1.0f - smoothstep(0.80f, 1.0f, t))
                                 * saturate(abs(speed) / kWaterFlowReference));
        }
    }

    float len = length(dir);
    s.direction = len > 1.0e-4f ? dir / len : g_flowParams.xy;
    return s;
}

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

    /// @note 流れの場が出す形。中心型は変位«後»の位置で評価する — 形は水平変位を持たないので、
    /// @note CPU の GetSurfaceHeightAt がワールド XZ をそのまま使うのと揃える。
    /// @note Baked だけは変位«前» (basePos) で引く。焼いた場は 3D なので、波を乗せた後の高さで
    /// @note 標本化すると «高さを高さで引く» 循環になる。
    int flowCount = (int)g_waveShapeParams.y;
    [loop]
    for (int fi = 0; fi < WATER_SURFACE_FLOW_COUNT; ++fi)
    {
        if (fi >= flowCount) continue;
        if ((int)g_surfaceFlowB[fi].z != WFF_BAKED)
        {
            float3 pit = WaterRadialDisplacement(fi, worldPos.xz);
            worldPos.y += pit.x;
            tangent.y  += pit.y;
            binormal.y += pit.z;
            continue;
        }
        /// @note 焼いた場に解析微分は無い。1 セルぶんの前進差分で勾配を取る。
        float  cell = max(max(g_timeParams.x, g_timeParams.y), 0.05f);
        float  h  = WaterBakedDisplacement(fi, basePos.xz);
        float  hx = WaterBakedDisplacement(fi, basePos.xz + float2(cell, 0.0f));
        float  hz = WaterBakedDisplacement(fi, basePos.xz + float2(0.0f, cell));
        worldPos.y += h;
        tangent.y  += (hx - h) / cell;
        binormal.y += (hz - h) / cell;
    }

    /// @note 着水の輪のうち «頂点で刻める» ぶん。CPU が B チャンネルへ meshFade 込みで焼いて
    /// @note いるので、ここは読んで足すだけ (water-waves.md の «波紋の帯分け»)。
    /// @note VS からのテクスチャ読みは SampleLevel。UV は波を乗せる前の頂点 UV で、CPU 側の
    /// @note GetSurfaceHeightAt も変位前の XZ で輪を引く。
    worldPos.y += (g_rippleTex.SampleLevel(g_samplerClamp, v.uv, 0).b * 2.0f - 1.0f)
                * kRippleHeightScale;

    /// @note 水平方向のヤコビアン。正規化前の tangent/binormal がそのまま ∂(x+d)/∂x の 2x2 になる。
    /// @note 1 を割るほど «波が前のめりに詰まっている»、負で «折り畳んでいる» = 崩れる波頭。
    /// @note 白波を高さで出すと山のてっぺんに丸く乗るが、実際の白波は波の前面に立つ。
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
    /// @note 真下を向き、NdotV が常に 0 → フレネル飽和・スペキュラ消失・影の反転を起こしていた。
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
    WaterFlowSample flowSample = SampleWaterSurfaceFlow(worldPos.xz, basePos.xz);
    o.flowDirectionDetail = float4(flowSample.direction, flowSample.detailGain, flowSample.chop);
    o.flowFoam = float2(flowSample.eye, flowSample.ring);
    return o;
}

/// @brief さざ波タイルから «1 ノイズセル単位の» 勾配と高さを引く。
/// @param q       ノイズセル単位の座標。
/// @param dqdx    q の画面 x 微分。異方フィルタの LOD 選択に使う。
/// @param dqdy    q の画面 y 微分。
/// @return xy = ∂h/∂q, z = 高さ [0,1]。
/// @note SampleGrad を使うのは、呼び出し元のオクターブループが break を持ち、勾配が非一様制御流れの
/// @note 中では未定義になるため。明示勾配なら break を残したまま正しい mip を選べる。
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
/// @param flowDir   この点の流れの向き (正規化)。渦の中では接線へ曲がる。
/// @param footprint このピクセルが覆うワールド距離 [m]。遠いほど・浅い角度ほど大きい。
/// @param dWorldDx  worldXZ の画面 x 微分。タイルの異方フィルタへ渡す。
/// @param dWorldDy  worldXZ の画面 y 微分。
/// @return xy = 勾配, z = Nyquist で落とした細部の割合 (0 = 全部残った, 1 = 全部落ちた)。
/// @note 風向に直交して座標を縮め、さざ波の «うね» を進行方向と直交させる。等方ノイズのままだと
/// @note 粒の集まりにしか見えず、水というより «ブツブツした膜» になる。
float3 WaterDetailGradient(float2 worldXZ, float2 flowDir, float time, float footprint,
                           float2 dWorldDx, float2 dWorldDy)
{
    float scale = max(g_detailParams.x, 1.0e-4f);
    float speed = g_detailParams.y;
    float2 flow = flowDir * (speed * time);

    float  stretch = max(g_flowParams.z, 1.0f);
    float2 alongFlow = flowDir;
    float2 crossFlow = float2(-alongFlow.y, alongFlow.x);
    float2x2 aniso = float2x2(alongFlow.x, alongFlow.y, crossFlow.x / stretch, crossFlow.y / stretch);

    /// @note オクターブごとに座標を回し、格子が縞として残らないようにする。異方スケールを
    /// @note 初期値に畳んでおけば、以降は回転を掛け足すだけで両方が同時に効く。
    float2x2 m = aniso;
    const float2x2 step = float2x2(0.80f, -0.60f, 0.60f, 0.80f);

    float2 grad = float2(0.0f, 0.0f);
    float amp = 1.0f, freq = scale, kept = 0.0f;

    [loop]
    for (int i = 0; i < WATER_DETAIL_OCTAVES; ++i)
    {
        /// @note 1 ピクセルに 1 周期以上入るオクターブは、平均すれば «ざらつき» しか残らない。
        /// @note 残すとカメラが動くたびに遠景の水面が総毛立って明滅する。周期がピクセルの 2 倍を
        /// @note 切ったところから滑らかに寝かせ、«細部が消えて鏡に近づく» 見え方へ収束させる。
        /// @note freq は単調増加なので、ここで潰れた先のオクターブはすべて潰れている。
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
/// @note 消すと «空を映すだけの板» になる。波長がピクセルより十分大きい間は傾きとして返せる。
/// @note 傾きの上限 1.2 (約 50 度) は安全弁。ak > 1 の波は本来砕けており、変位を伴わないここでは
/// @note そのままの傾きを出すと «壁» に見える。
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
/// @param detailGain 流れの場が持ち上げるさざ波の強度倍率 (1 起点)。
/// @return xyz = 接空間法線, w = 落とした細部量 (specular AA で粗さへ回す)。
/// @note うねりの勾配でさざ波の座標をずらす (領域ワープ)。波が «さざ波を運ぶ» ので、ワープが無いと
/// @note うねりとさざ波が別々の層として滑って見える。ワープ量は画面上でゆっくり変わるため、
/// @note 異方フィルタへ渡す微分には含めていない。
float4 SampleWaterNormal(float2 worldXZ, float2 uv, float2 flowDir, float time, float footprint,
                         float2 dWorldDx, float2 dWorldDy, float3 geoNormal, float detailGain)
{
    float2 swellSlope = -geoNormal.xz / max(geoNormal.y, 0.2f);
    float2 warped = worldXZ + swellSlope * g_flowParams.w;

    float3 detail = WaterDetailGradient(warped, flowDir, time, footprint, dWorldDx, dWorldDy);
    /// @note 風上を向いた斜面ほどさざ波が立ち、風下は凪ぐ。一様に散らすとうねりが «ただの起伏» に見える。
    float windward = saturate(0.5f + dot(swellSlope, flowDir));
    float2 grad = detail.xy * max(g_detailParams.z, 0.0f) * lerp(0.55f, 1.35f, windward)
                * max(detailGain, 0.0f);

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
/// @note 縁へ向かって消していけば «岸に向かって薄くなる水» として読める。
/// @note 帯の幅はメートル固定なので、海サイズでは縁が遠すぎて 1 ピクセルに収まる。画面上で数ピクセル
/// @note ぶんを下支えすれば、どんな大きさでも縁は溶けたまま消える。上限 0.05 / 0.15 は、寝た視線で
/// @note 遠景の水がまとめて消えるのと、小さな水面の中央まで薄まるのを防ぐ歯止め。
float WaterEdgeFade(float2 uv, float2 widthUV, float2 extent, float footprint)
{
    float2 minWidth = min(footprint * 4.0f / max(extent, 1.0e-4f), 0.05f);
    float2 w = min(max(widthUV, minWidth), 0.15f);
    float2 d = min(uv, 1.0f - uv); ///< @brief 最も近い縁までの距離 [0, 0.5]
    float fx = widthUV.x > 1.0e-5f ? smoothstep(0.0f, w.x, d.x) : 1.0f;
    float fy = widthUV.y > 1.0e-5f ? smoothstep(0.0f, w.y, d.y) : 1.0f;
    return fx * fy;
}

/// @brief 深度バッファの生値 (Reversed-Z: near → 1、far → 0) を視空間 Z へ直す。
/// @note 平行投影では深度が既に線形。透視用の逆数式を通すと水深フェード・屈折・水面 SSR が一斉にずれる。
/// @see Common/Space.hlsli の LinearizeDepth (同じ式)
float LinearizeDepth(float rawDepth)
{
    if (isOrthographic > 0.5f) return farZ - rawDepth * (farZ - nearZ);
    return (nearZ * farZ) / max(nearZ + rawDepth * (farZ - nearZ), 0.0001f);
}

/// @brief 水面専用 SSR。
/// @return rgb = 反射色, a = 信頼度 (0 でヒット無し)。
/// @note Water 描画直前の sceneDepth / sceneColor を使うため、現在の水面を読み戻す競合を起こさず、
/// @note 既に描画済みの不透明・半透明オブジェクトを反射できる。
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
        /// @note 空 (Reversed-Z で深度 0) は反射の当たり先にしない。
        if (sceneRawDepth <= 0.0f)
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

    /// @note 場の評価は頂点で行う。補間した向きは渦の中心でゼロになり得るため安全に正規化する。
    WaterFlowSample flowSample;
    float flowLength = length(p.flowDirectionDetail.xy);
    flowSample.direction = flowLength > 1.0e-4f ? p.flowDirectionDetail.xy / flowLength : g_flowParams.xy;
    flowSample.detailGain = p.flowDirectionDetail.z;
    flowSample.chop = p.flowDirectionDetail.w;
    flowSample.eye = p.flowFoam.x;
    flowSample.ring = p.flowFoam.y;
    float2 flowDir = flowSample.direction;

    float4 normalSample  = SampleWaterNormal(p.worldPos.xz, p.uv, flowDir, time, footprint,
                                             footprintDX, footprintDY, p.normal,
                                             flowSample.detailGain);
    float3 tangentNormal = normalSample.xyz;
    float  lostDetail    = normalSample.w;

    float3x3 tbn = float3x3(normalize(p.tangent), normalize(p.binormal), normalize(p.normal));
    float3 N = normalize(mul(tangentNormal, tbn));
    float3 V = normalize(cameraPos - p.worldPos);
    /// @note 水面下から見上げているときは面の裏側を見ている (PSO は両面描画)。
    /// @warning 向きの判定に N を使うと、浅い角度のピクセルだけがばらばらに反転して斑になる。
    /// @note 決定は滑らかな幾何法線に任せる。
    if (dot(p.normal, V) < 0.0f) N = -N;
    float NdotV = saturate(dot(N, V));

    /// @note Schlick フレネル。g_surfaceParams.z を水の F0 (実測 0.02 前後) として扱う。
    /// @warning 以前の bias + (1-bias)*pow は grazing 角以外でも下駄を履かせており、真上から見た
    /// @note 水面まで一定量の反射が乗って «板に空が映っている» 見え方になっていた。
    /// @see https://doi.org/10.1111/1467-8659.1330233 Schlick, "An Inexpensive BRDF Model for Physically-based Rendering" (1994)
    float f0 = saturate(g_surfaceParams.z);
    float fresnel = f0 + (1.0f - f0) * pow(saturate(1.0f - NdotV), max(g_surfaceParams.w, 1.0f));
    fresnel = saturate(fresnel * g_surfaceParams.y * 2.0f);

    float rawSceneDepth = g_sceneDepth.Sample(g_samplerClamp, screenUV).r;
    /// @note 深度が far plane に張り付く場所は Terrain / Mesh が無い背景ピクセルとして扱う。
    /// @note 背景の skydome 色を屈折色として読むと、水面が空そのものに溶けてしまうため、
    /// @note «底が見えない深い水» として描く。
    /// @note 深度は Reversed-Z なので背景 (クリア値のまま・空) は 0。
    float backgroundMask = rawSceneDepth <= 0.0f ? 1.0f : 0.0f;
    float linearSceneDepth = LinearizeDepth(rawSceneDepth);
    float linearSurfDepth = max(p.screenPos.w, 0.0001f);
    float waterDepth = lerp(max(0.0f, linearSceneDepth - linearSurfDepth), g_deepColorDepth.w, backgroundMask);
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

    /// @note スクリーンスペース屈折は水面法線で HDR カラー参照 UV をずらす。水底ジオメトリを
    /// @note 再描画せず透明水面らしい歪みを得る。現在描画中の HDR RT を直接読むと read/write
    /// @note 競合になるため、Water 直前にコピーした sceneColor を参照する。
    float refractionMask = shallowFactor * (1.0f - backgroundMask);
    float2 refrOffset = tangentNormal.xy * g_refractionFlowParams.x * (1.0f - saturate(fresnel)) * refractionMask;
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
    /// @warning 反射ベクトルを水平線より下へ向けないこと。遠い水面ほど視線が寝て R は水平線すれすれを
    /// @note 向き、さざ波が 1 つ揺れるだけで R が下半球 (ほぼ真っ黒) へ落ちて黒い帯が出る。
    float3 R = reflect(-V, N);
    R = normalize(float3(R.x, max(R.y, 0.02f), R.z));
    /// @note 落とした細部はサブピクセルの法線ばらつきそのものなので粗さへ移す (specular AA)。
    /// @note 移さないと «消えた細部» が反射とハイライトからだけ抜け落ち、遠景の水面が磨いた金属板になる。
    float  roughness = saturate(1.0f - g_detailParams.w);
    float  roughnessAA = saturate(roughness + lostDetail * (1.0f - roughness) * 0.70f);
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
        reflectColor = lerp(reflectColor, ssrReflection.rgb, ssrReflection.a);
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
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        float3 punctualRadiant = ps.color * (ps.intensity * LIGHT_UNIT_SCALE);
        float  punctualNdotL   = saturate(dot(N, ps.L));
        punctualOverPi   += punctualRadiant * (punctualNdotL * INV_PI);
        punctualSpecular += BRDF_Specular(N, V, ps.L, f0.xxx, saturate(roughnessAA + ps.roughnessBias))
                          * punctualRadiant * punctualNdotL;
    FBZZ_PUNCTUAL_END

    /// @note 水の層が返す光 = 散乱色 × (太陽 + 空 + 局所光)。定数色のまま出すと昼夜・影に追随しない。
    float3 bodyIrradiance = sunOverPi * (sunCosFlat * shadow) + skyIrradianceOverPi + punctualOverPi;
    float3 inscatter = scatterAlbedo * bodyIrradiance;
    float3 body = lerp(refractColor * transmittance, inscatter, murk);

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
    /// @note 峰を広げたぶんだけ (α/α')^2 で落とす。これを掛けないと «広げた» のではなく
    /// @note «明るくした» ことになり、光のエネルギーが増える。
    float sunAlpha = max(roughnessAA * roughnessAA, 1.0e-5f);
    float sunAlphaWide = saturate(sunAlpha + 0.0023f);
    float sunEnergy = (sunAlpha / sunAlphaWide) * (sunAlpha / sunAlphaWide);
    float NdotL = saturate(dot(N, L));
    /// @see https://www.graphics.cornell.edu/~bjw/microfacetbsdf.pdf Walter et al., "Microfacet Models for Refraction through Rough Surfaces" (2007) §5.2 GGX
    float3 sunSpecular = BRDF_SpecularAdvanced(N, V, L, f0.xxx, sqrt(sunAlphaWide),
                                               specT, specB, specAniso)
                       * (sunEnergy * NdotL * shadow * PI) * sunOverPi;

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

    /// @note 透過した水の層と反射をフレネルで分け合い、表面のきらめきは上に足す。
    float3 color = lerp(body, reflectColor, reflectionWeight)
                 + (sunSpecular + punctualSpecular) * specularStrength;

    /// @note 波打ち際 (地形と水面が交差する浅瀬) に発生する接岸泡。waterDepth が threshold より
    /// @note 浅いほど強くし、岸辺に沿った白い帯を作る。
    float foamThreshold = g_foamParams.x;
    float foamFade      = max(g_foamParams.y, 0.0001f);
    float shoreFoam     = (1.0f - smoothstep(foamThreshold, foamThreshold + foamFade, waterDepth)) * (1.0f - backgroundMask);

    float foamMaskVal = g_foamMask.Sample(g_samplerClamp, p.uv).r;
    /// @note 泡のムラもさざ波タイルから引く。ワールド座標なので水面の大きさに依らず粒が揃う。
    float foamScale = max(g_foamParams.w, 1.0e-4f);
    float2 foamP = p.worldPos.xz * foamScale + flowDir * (time * 0.35f);
    /// @note 乱流 (Curl) はムラのコントラストを上げる。«ざわついた水» は泡の粒が揃わない。
    float foamTexVal = saturate(WaterNoiseTile(foamP, footprintDX * foamScale,
                                               footprintDY * foamScale).z
                                * lerp(1.6f, 3.0f, flowSample.chop));
    /// @note 崩れる白波は «Gerstner 変位が折り畳む» ところに出る。岸が無い外洋でも波が立って
    /// @note 見えるかはここで決まる。山の高さで出すと白が波頭に丸く乗り、波が «進んで» 見えない。
    float crestFoam = smoothstep(0.55f, 1.0f, p.waveState.y) * 0.85f;
    /// @note 渦と排水口の目は実際に白く泡立つ。足さないと «ただ凹んだ穴» にしか見えない。
    /// @note 湧き出しは縁で水がぶつかるので、輪として出す。
    float eyeFoam = max(flowSample.eye * 0.9f, flowSample.ring * 0.7f);
    float foamAmount = max(max(max(foamMaskVal, shoreFoam), crestFoam), eyeFoam) * foamTexVal;
    float foam = saturate(smoothstep(0.05f, 1.0f, foamAmount) * g_foamParams.z);
    /// @note 泡と波紋の輪は «水面に浮いた白い粗面» なので、定数色ではなく Lambert で照らす。
    /// @note 太陽は泡の面の向きで受け、影の中では空と局所光だけが残る。
    float3 surfaceIrradiance = sunOverPi * (NdotL * shadow) + skyIrradianceOverPi + punctualOverPi;
    float3 foamAlbedo = lerp(float3(0.72f, 0.88f, 0.92f), float3(1.0f, 1.0f, 1.0f), saturate(foamTexVal));
    color = lerp(color, foamAlbedo * surfaceIrradiance, foam);

    float2 rippleRG = g_rippleTex.Sample(g_samplerClamp, p.uv).rg * 2.0f - 1.0f;
    float rippleRing = saturate(length(rippleRG) * rippleRingStrength);
    color = lerp(color, rippleRingColor * surfaceIrradiance, rippleRing);
    /// @warning ここで色を事前圧縮しないこと。水面は HDR バッファへブレンド描画され、露出・ACES
    /// @note トーンマップは Composite パスが一括で行う。事前圧縮するときらめきが Bloom に乗らない。
    color = max(color, 0.0f);

    /// @note 屈折と濁りはこの色の中で合成済み。ブレンドで背後をもう一度混ぜると、水底が二重に
    /// @note 入って反射ときらめきが薄まるので、アルファは外周フェードだけに使う。
    /// @note 縁ではコピー前のシーンと同じ背後へ溶けるので、屈折を合成済みでも継ぎ目は出ない。
    float alpha = WaterEdgeFade(p.uv, g_refractionFlowParams.zw, g_normalParams.xy, footprint);
    /// @note 水面は自分自身の重なりを解決するため深度を書く。フェードで見えなくなった縁がそのまま
    /// @note 深度を書くと、その裏の半透明やデカールを «見えない板» が遮る。絵に出ない画素は残さない。
    clip(alpha - 0.003f);
    return float4(color, alpha);
}
