/// @file    EnergyLance.hlsl
/// @brief   電磁の «柱 / 槍 / 扇»。1 枚の帯を «場が立ち上がっている管» として描く
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// PSO: SOLID_NOCULL + PREMULTIPLIED + DEPTH_READ
///
/// 頂点は LineRendererComponent が組む帯で、uv.x = 根元から先端への進み [0,1]、
/// uv.y = 帯の横断 [0,1]。柱は «下から上» へ 2 点を張った帯、槍は «頭から着弾点» へ
/// 張った帯で、どちらも同じ 1 枚として解ける。
///
/// WHY ボスのコアビーム (BossBeam.hlsl) を使い回さないか:
///   あちらは «切っている棒» ─ 焦点環で絞り込んだレンズの筒として、当たったら終わりの
///   一撃を表している。蛇のこれは «帯電した場が立ち上がっている» 側で、読ませたいのは
///   «今そこに居てはいけない» という場所の宣言。芯の鋭さではなく、**軸を走る輪**と
///   **溜めで針まで絞れること** が要件になる。同じシェーダーへ両方を入れると、
///   片方の «質» を触るたびにもう片方が動く。
///
/// WHY «輪» が要るか (縞のスクロールだけでは足りない):
///   柱は縦に立つので、長さ方向へ模様を流しても «上か下か» が分かるだけで、
///   太さの変化が読めない。軸に直交する輪が下から上へ抜けると、柱そのものが
///   «エネルギーを送り出している管» に見える。輪は uv.x だけで作れるので、
///   槍として横に張ったときは «銃口から先端へ抜ける脈» として同じ絵が働く。
///
/// WHY 溜め (charge) を絵の側に持つか:
///   予兆は «細い針が一瞬で本径へ太る» が最も読みやすい。太さを AI が毎フレーム
///   書く形にすると «予兆の作り方» が撃つ側に散らばる。ここが charge 1 つで
///   «芯の細さ・輪の密度・明るさ» をまとめて動かせば、撃つ側は 0→1 を渡すだけで済む。
///
/// WHY 事前乗算 (PREMULTIPLIED) か:
///   芯は背景を隠して光り、外周は背景へ足すだけ。ALPHA_BLEND では縁が背景を薄めて
///   濁り、ADDITIVE では芯まで透けて «柱の中心» が読めない (Beam.hlsl と同じ判断)。
///
/// WHY cbuffer の名前を変えてはいけないか:
///   ShaderDescriptor は PS バイトコードを GetConstantBufferByName("MaterialConstants")
///   で引いて変数表を作る。名前が違うと表が空のまま «有効な Descriptor» が返り、
///   MaterialInstance::Set* の per-instance 上書きが全部黙って捨てられる。

#include "Common/Binding.hlsli"

#define FBZZ_MATERIAL_CONSTANTS
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    /// 線の色と «背景を隠す量»。LineRenderer が startColor から毎フレーム書く。
    float4 albedo;

    // ── 断面 ────────────────────────────────────────────────────────────────
    /// 芯の太さ。帯の半幅に対する割合 [0,1]。
    float  coreWidth;
    /// 縁の減衰指数。大きいほど芯が細く鋭くなる。
    float  edgeFalloff;
    /// 芯の明るさ倍率。
    float  coreBoost;
    /// 全体の明るさ (HDR)。
    float  intensity;

    // ── 流れ ────────────────────────────────────────────────────────────────
    /// 素材を長さ方向へ何回繰り返すか。0 で 1 枚を引き伸ばす。
    float  tiling;
    /// uv.x のスクロール。スクリプトが毎フレーム進める。
    float  scroll;
    /// 根元の立ち上がり [0,1]。0 で切り口がそのまま出る。
    float  muzzleFade;
    /// 先端の減衰 [0,1]。
    float  tipFade;

    // ── 輪 ──────────────────────────────────────────────────────────────────
    /// 軸に直交する輪の数 [本/帯]。
    float  ringDensity;
    /// 輪が根元から先端へ抜ける速さ [周/秒相当]。phase に掛かる。
    float  ringSpeed;
    /// 輪の太さ [0,1]。小さいほど鋭い線になる。
    float  ringWidth;
    /// 輪の明るさ。芯より明るいと «輪だけの筒» に見えるので控えめに。
    float  ringBoost;

    // ── 溜めと乱れ ──────────────────────────────────────────────────────────
    /// 位相。スクリプトが毎フレーム進める。
    float  phase;
    /// 0 = 針 (予兆) / 1 = 本径。芯の太さ・輪の密度・明るさをまとめて動かす。
    float  charge;
    /// 明滅の深さ [0,1]。
    float  flicker;
    /// 帯の外へにじむ熱。柱の «場» を出すが、背景は隠さない (純加算)。
    float  heatWash;
};

// WHY cbuffer の «後» か: Constants.hlsli は FBZZ_MATERIAL_CONSTANTS が立っていないと
//     既定の MaterialConstants を宣言する。上で自前の表を宣言してから読むこと。
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"

Texture2D    texAlbedo   : register(TEX_ALBEDO);
SamplerState sampDefault : register(SAMPLER_DEFAULT);

static const float kLaserTau = 6.28318530718f;

struct LaserPSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
    /// 帯の進行方向 (ワールド)。LineRenderer が区間の向きを tangent へ入れている。
    float3 axis       : TEXCOORD1;
    /// 画素からカメラへ向かうベクトル (ワールド、非正規化)。
    float3 toEye      : TEXCOORD2;
};

LaserPSIn VSMain(VSInput v)
{
    LaserPSIn o;
    float4 worldPos = mul(float4(v.position, 1.0f), world);
    o.svPosition = mul(worldPos, viewProjection);
    o.uv    = v.uv;
    o.axis  = mul(v.tangent, (float3x3)world);
    o.toEye = cameraPos - worldPos.xyz;
    return o;
}

/// 帯を «円柱の断面» として読む。視線が円柱を抜ける弦の長さ ─ 中心が濃く縁が薄い。
///
/// WHY 単なるグラデーションにしないか: 直線の減衰は «光っている板» にしかならない。
///     弦の長さ (sqrt(1 - r^2)) は体積の分布そのものなので、同じ 1 枚が管に見える。
float ChordThickness(float across)
{
    const float r = saturate(abs(across));
    return sqrt(saturate(1.0f - r * r));
}

float4 PSMain(LaserPSIn input) : SV_Target0
{
    // 断面 (-1..1) と長さ (0..1)。
    const float across = input.uv.y * 2.0f - 1.0f;
    const float along  = saturate(input.uv.x);

    // 溜め。針から本径へ。0 でも完全には消さない ─ 消えると予兆が «無い» になる。
    const float fill = lerp(0.14f, 1.0f, saturate(charge));

    // ── 芯 ──
    // 芯の «幅» を溜めで絞る。太さを外から書かず、ここ 1 つで予兆が作れる。
    const float halfWidth = max(coreWidth * fill, 1e-3f);
    const float coreMask  = pow(saturate(1.0f - abs(across) / halfWidth), max(edgeFalloff, 0.1f));
    const float volume    = ChordThickness(across);

    // ── 輪 ──
    // 軸に沿って走る帯。柱として立てたとき «下から上へ» 抜けていく。
    const float ringPhase = along * max(ringDensity, 0.0f) - phase * ringSpeed;
    const float ringWave  = sin(ringPhase * kLaserTau) * 0.5f + 0.5f;
    // 尖らせる。sin のままだと «太い縞» で、輪として読めない。
    const float ring = pow(ringWave, max(1.0f / max(ringWidth, 0.02f), 1.0f));

    // ── 素材 ──
    // 長さ方向へタイル + スクロール。素材が無い構成でも成立するよう 1 で割らない。
    const float2 uv  = float2(along * max(tiling, 0.0f) + scroll, input.uv.y);
    const float  tex = texAlbedo.Sample(sampDefault, uv).r;

    // ── 端の始末 ──
    // 根元と先端は必ず 0 へ落とす。切り口が出ると «棒を置いた» に見える。
    const float ends = smoothstep(0.0f, max(muzzleFade, 1e-3f), along)
                     * smoothstep(0.0f, max(tipFade, 1e-3f), 1.0f - along);

    // ── 明滅 ──
    // 溜めのあいだだけ強く震わせる。撃ち始めたら落ち着く ─ 撃っている間ずっと
    // 震えていると «不安定なもの» に見えて、避ける対象として読みにくい。
    const float shiver = 1.0f - flicker * (1.0f - saturate(charge))
                       * (sin(phase * 37.0f) * 0.5f + 0.5f);

    // 芯 + 輪 + 素材の粒。輪は芯の «外側» でだけ効かせる (中心で重ねると白飛びする)。
    const float shell = saturate(volume - coreMask);
    float luminance = coreMask * max(coreBoost, 0.0f)
                    + shell * ring * max(ringBoost, 0.0f)
                    + volume * (0.35f + 0.65f * tex) * 0.45f;
    luminance *= ends * shiver * max(intensity, 0.0f);

    // 熱のにじみ。帯の外側だけに乗せ、背景は隠さない。
    const float wash = saturate(1.0f - abs(across)) * max(heatWash, 0.0f) * fill * ends;

    // 事前乗算。a は «隠す量» なので、芯だけが背景を隠す。
    const float3 color = albedo.rgb * (luminance + wash);
    const float  alpha = saturate(coreMask * volume * albedo.a * ends * fill);
    return float4(color, alpha);
}
