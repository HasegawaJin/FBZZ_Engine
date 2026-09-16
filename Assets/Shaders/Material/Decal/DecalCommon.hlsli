/// @file DecalCommon.hlsli
/// @brief デカールシェーダーの共通契約。デカール用シェーダーは必ずこれを include する
/// @author Hasegawa Jin
/// @date 2026-08-23
///
/// WHY 契約をファイルにするか:
///   デカールは「フルスクリーントライアングルを 1 枚描き、深度からワールド座標を
///   復元して OBB へ投影する」という定型が絵の内容と無関係に必ず要る。これを
///   シェーダーごとに写経させると、血痕を 1 種類足すだけで復元・角度フェード・
///   受信レイヤー判定を全部書き写すことになり、写し間違えても「出ない」以外の
///   情報が出ない。定型は DecalResolve() に閉じ、材質側は PSMain だけ書く。
///
/// WHY マテリアルパラメータを MaterialConstants (b2) に置くか:
///   BuildDescriptor() は cbuffer を **名前** で探す
///   (GetConstantBufferByName("MaterialConstants"))。この名前とレジスタに
///   合わせておくだけで、既存の .mat / ShaderDescriptor / Inspector が
///   そのままデカールにも効く。デカール専用の反射経路を作る必要が無い。
///   エンジンが埋める投影データは CB_DECAL (b10) へ逃がしてある。
#ifndef DECAL_COMMON_HLSLI
#define DECAL_COMMON_HLSLI

#ifdef CONSTANTS_HLSLI
#error "Decal shaders must not include Common/Constants.hlsli: it declares MaterialConstants at b2, which decal materials own. Include Material/Decal/DecalCommon.hlsli only."
#endif

#include "Common/Binding.hlsli"
#include "Common/Space.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texDecalDepth, TEX_DEPTH_SLOT);
FBZZ_TEX2D(texDecalReceiver, TEX_DECAL_MASK_SLOT);
SamplerState sampDecal        : register(SAMPLER_DEFAULT);

cbuffer CameraConstants : register(CB_CAMERA)
{
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float3   cameraPos;
    float    nearZ;
    float    farZ;
    float    _camReserved;   // Water パスのみ waterSsrEnabled として使う枠
    float    isOrthographic; // 1 = 平行投影
    float    _camPad;
};

// デカールパスは DeferredLighting 後の HDR へ合成するため GBuffer 法線を更新できない。
// 法線マップの陰影を「相対的な明暗差」として乗せるのに主平行光だけ要る。
cbuffer LightConstants : register(CB_LIGHT)
{
    float3 lightDir;
    float  _lightPad;
    float3 lightColor;
    float  lightIntensity;
};

// DecalPass が毎ドロー埋める。材質側からは読み取り専用。
// LAYOUT: Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp の DecalCB と一致させること。
cbuffer DecalConstants : register(CB_DECAL)
{
    float4x4 invDecalWorld;      // ワールド → デカールローカル ([-0.5, 0.5]^3)
    float3   decalTangent;       // デカールローカル +X (法線マップの T 軸)
    float    _decalPad0;
    float3   decalBitangent;     // デカールローカル +Z (投影 UV の V 軸)
    float    _decalPad1;
    float3   decalNormal;        // 投影軸 (デカールローカル +Y)
    float    _decalPad2;
    float    decalAlpha;         // ライフタイムフェード込みの不透明度
    float    angleFadeStrength;  // 0 = 角度フェードなし
    float    angleFadeCos;       // この cos より寝た面では完全に消える
    uint     decalFlags;         // bit0 = 受信レイヤーフィルタ有効
    uint     decalReceiverMask;  // 受信を許すレイヤーのビットマスク
    float2   decalFrameScale;    // フリップブック 1 コマぶんの UV スケール (無効なら (1,1))
    float    decalFrameIndex;    // 今のコマ番号 (0 起点)
};

#define DECAL_FLAG_RECEIVER_FILTER 1u

// 材質パラメータを公開したいシェーダーだけが宣言する。宣言する場合は必ず
// この名前とレジスタを使う (組み込みの Decal.hlsl が最小の実例)。
//
//   cbuffer MaterialConstants : register(CB_MATERIAL)
//   {
//       float4 albedoTint;   // offset  0
//       ...
//       uint   textureMask;  // Material::Upload がスロットの有効性から埋める
//   };

struct DecalPixelInput
{
    float4 pos      : SV_POSITION;
    float2 screenUv : TEXCOORD0;
};

// 全デカールシェーダーで共通の頂点処理。頂点バッファは持たない。
// 呼び出し側の VSMain が SV_VertexID を受け取ってそのまま渡す。
DecalPixelInput DecalVertexMain(uint id)
{
    DecalPixelInput output;
    output.screenUv = float2((id & 1u) ? 2.0f : 0.0f,
                             (id & 2u) ? 2.0f : 0.0f);
    output.pos      = float4(output.screenUv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f),
                             0.0f, 1.0f);
    return output;
}

// DecalResolve() が返す、この画素へデカールを乗せるのに要る値一式。
struct DecalSurface
{
    float2 uv;             // デカールローカル XZ 平面へ投影した UV
    float3 worldPos;       // 受け面のワールド座標
    float3 receiverNormal; // 受け面のワールド法線
    float  alpha;          // ライフタイム × 角度フェード
};

/// 深度から受け面を復元し、この画素にデカールが乗るかを判定する。
/// @return false ならこの画素は捨てる (呼び出し側で discard する)
///
/// IMPORTANT: ddx / ddy はクワッド内の隣接ピクセルを参照するため、discard した
///   レーンが混ざると結果が未定義になる。微分に使う値は全ての棄却より前で
///   計算しきってあり、この関数自身は discard しない。
bool DecalResolve(float2 screenUv, out DecalSurface surface)
{
    surface = (DecalSurface)0;

    float  ndcDepth = texDecalDepth.Sample(sampDecal, screenUv).r;
    float3 worldPos = ReconstructWorldPos(screenUv, ndcDepth, invViewProjection);
    // 受け面の法線はスクリーン空間微分から求める。
    // WHY GBuffer 法線を使わないか: このパスは DeferredLighting 後の HDR へ合成し、
    //     Forward 経路でも走るため、GBuffer が読める保証が無い。
    // NOTE: 深度の不連続 (シルエット境界) では微分が跳ねる。そこは角度フェードで
    //       薄くなる側へ倒れるだけなので、破綻としては安全側。
    float3 receiverNormal = normalize(cross(ddy(worldPos), ddx(worldPos)));

    surface.worldPos       = worldPos;
    surface.receiverNormal = receiverNormal;

    // 空 / 背景
    if (ndcDepth >= 1.0f)
        return false;

    if (decalFlags & DECAL_FLAG_RECEIVER_FILTER)
    {
        // バッファには可視サーフェスのレイヤー番号 + 1 が入る。0 は「不明」で、
        // 受信バッファへ描かないジオメトリ (地形・フォリッジ等) が該当する。
        // 不明を除外側へ倒すとそれらへ一切デカールが乗らなくなるため受信させる。
        uint encoded = (uint)(texDecalReceiver.Sample(sampDecal, screenUv).r + 0.5f);
        if (encoded != 0u && (decalReceiverMask & (1u << ((encoded - 1u) & 31u))) == 0u)
            return false;
    }

    float3 localPos = mul(float4(worldPos, 1.0f), invDecalWorld).xyz;
    if (any(abs(localPos) > 0.5f))
        return false;

    // ローカル -Y が投影軸。XZ 平面へ落として UV にする。
    surface.uv = localPos.xz + 0.5f;

    // --- 角度フェード -----------------------------------------------------------
    //
    // WHY: OBB 投影は投影軸に対して斜めな面へ当てるとテクスチャが引き伸ばされ、
    //      長い筋になる。着弾痕が壁と床の角をまたいだ瞬間に「伸びた汚れ」として
    //      露見する、デカールで最も目立つ破綻がこれ。角度で薄めれば破綻する範囲が
    //      そのまま消える (Unity / Unreal の Angle Fade と同じ考え方)。
    //
    // 表裏どちらの向きでも同じ扱いにしたいので abs を取る。
    float facing = abs(dot(receiverNormal, normalize(decalNormal)));
    float angleFactor = angleFadeCos < 1.0f
        ? saturate((facing - angleFadeCos) / max(1.0f - angleFadeCos, 1.0e-3f))
        : 1.0f;
    // 端で硬く切れると縁が線として見えるため、なめらかに落とす。
    angleFactor = angleFactor * angleFactor * (3.0f - 2.0f * angleFactor);

    surface.alpha = decalAlpha * lerp(1.0f, angleFactor, saturate(angleFadeStrength));
    return surface.alpha >= 0.001f;
}

/// フリップブックの現在コマへ UV を写す。無効なデカールでは素通し。
///
/// WHY オフセットを受け取らず段数から組むか: DecalConstants は旧 pad の 3 float に
///     収めてある (スケール 2 + 番号 1)。オフセットを別に持たせると cbuffer が
///     1 レジスタ伸び、この .hlsli の 4 つのコピーと C++ 側の static_assert を
///     «全部同時に» 直さないと、直し忘れた側が黙って別の値を読む。
///
/// NOTE: コマの境界ではバイリニアが隣のコマを拾う。アトラスは各コマの周囲に
///       1〜2 画素の余白を持たせて作ること。
float2 DecalApplyFlipbook(float2 uv)
{
    // スケールから «横のコマ数 / 縦の段数» を復元する。無効時は (1, 1) なので素通し。
    float2 tiles = round(1.0f / max(decalFrameScale, 1.0e-6f));
    if (tiles.x <= 1.0f && tiles.y <= 1.0f)
        return uv;

    float column = fmod(max(decalFrameIndex, 0.0f), max(tiles.x, 1.0f));
    float row    = floor(max(decalFrameIndex, 0.0f) / max(tiles.x, 1.0f));
    // frac は «タイリングを掛けた UV がコマから溢れる» のを防ぐためのもの。
    return (frac(uv) + float2(column, row)) * decalFrameScale;
}

/// 接空間法線をデカールの投影基底でワールドへ移す。
float3 DecalDecodeNormal(float3 normalSample)
{
    float3 tangentNormal = normalSample * 2.0f - 1.0f;
    return normalize(normalize(decalTangent)   * tangentNormal.x
                   + normalize(decalBitangent) * tangentNormal.y
                   + normalize(decalNormal)    * tangentNormal.z);
}

/// 法線マップの陰影を受け面比の明暗差として返す。
///
/// WHY 絶対値でなく比か: このパスは DeferredLighting の後で HDR へ合成するので、
///     GBuffer 法線を書き換えて正規のライティングを通すことができない。差分だけを
///     掛ければ、平坦な法線マップは元の明るさを保ったまま凹凸だけが乗る。
float DecalNormalLightRatio(float3 baseNormal, float3 mappedNormal)
{
    float3 L = normalize(-lightDir);
    float baseLight   = 0.25f + saturate(dot(normalize(baseNormal),   L)) * max(lightIntensity, 0.0f);
    float mappedLight = 0.25f + saturate(dot(normalize(mappedNormal), L)) * max(lightIntensity, 0.0f);
    return clamp(mappedLight / max(baseLight, 0.001f), 0.25f, 2.0f);
}

#endif // DECAL_COMMON_HLSLI
