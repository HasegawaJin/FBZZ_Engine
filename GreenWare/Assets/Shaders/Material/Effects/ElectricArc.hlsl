/// @file    ElectricArc.hlsl
/// @brief   電極間の放電リボン。芯・グロー・途切れ・極性グラデーションを 1 枚で描く。
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// PSO: SOLID_NOCULL + ALPHA_BLEND + DEPTH_READ
///
/// 頂点は LineRendererComponent が組む帯で、uv.x = 始点から終点への進み [0,1]、
/// uv.y = 帯の横断 [0,1]。断面の形も途切れも、この 2 軸だけで作る。
///
/// WHY 時間を cbuffer から取らないか:
///   time を持つのは PostProcConstants (b6) で、ジオメトリ描画では束縛されない。
///   ちらつきの位相は phase パラメーターとしてスクリプトが毎フレーム書き込む。
///   結果として「放電が走るタイミング」がスクリプト側の 1 箇所に集まり、
///   稲妻の形・明るさ・音を同じ乱数で揃えられる。
///
/// WHY 加算合成でなくアルファ合成を選ぶか:
///   RGB を HDR (1 超) で出し、芯は不透明な白として背景を隠す。縁だけがアルファで溶ける。
///   加算よりフィラメントらしい硬さが出るうえ、明るさはそのままブルームへ乗る。
///   NOTE: 以前はこれが «選択» ではなく «強制» だった。applyMaterial が LineRenderer の
///         blendMode を毎フレーム ALPHA_BLEND で焼き付けていたため、.mat で加算を選んでも
///         通らなかった。今は .mat の blend_mode が正本なので、ここを加算や事前乗算へ
///         変えたければ ElectricArc.mat 側を書き換えれば効く
///         (事前乗算で «足す光と隠す量» を分ける例は同フォルダの Beam.hlsl)。
///
/// WHY cbuffer の名前を変えてはいけないか:
///   ShaderDescriptor は PS バイトコードを GetConstantBufferByName("MaterialConstants")
///   で引いて変数表を作る (DX11Shader::BuildDescriptor)。名前が違うと表が空のまま
///   «有効な Descriptor» が返り、MaterialInstance::Set* が «型が合わない» として
///   全部の per-instance 上書きを黙って捨てる (.mat の既定値だけで描かれる)。
///   register(CB_MATERIAL) を合わせるだけでは足りず、名前が一致していることが条件。

#include "Common/Binding.hlsli"

#define FBZZ_MATERIAL_CONSTANTS
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    /// 始点 (＋極) 側の色と全体の不透明度。LineRenderer が startColor から毎フレーム書く。
    float4 albedo;
    /// 芯の色。白熱させるため HDR で 1 を大きく超える値を入れる。
    float4 coreColor;
    /// 終点 (−極) 側の色。albedo との間を uv.x で補間して極性の対を見せる。
    float4 tipColor;

    /// 芯の太さ。帯の半幅に対する割合 [0,1]。
    float  coreWidth;
    /// 縁の減衰指数。大きいほど芯が細く鋭くなる。
    float  glowFalloff;
    /// 全体の明るさ倍率 (HDR)。放電の強弱はここで振る。
    float  intensity;
    /// ちらつきの位相。スクリプトが毎フレーム進める。
    float  phase;

    /// 芯の途切れ量 [0,1]。1 に近いほど筋が断続する。
    float  breakup;
    /// 両端の侵食 [0,1]。1 で端が完全に消える。走り始め / 消え際に使う。
    float  erode;
    /// 帯に沿って流れる輝点の速さ。0 で流れない。
    float  travel;
    /// 芯をレール色へ寄せる量 [0,1]。0 で純白の芯、1 で完全に極性色。
    ///
    /// WHY 芯を純白のままにしないか:
    ///   芯が白いままだと、太くしたり本数を増やしたりした瞬間に
    ///   «白い帯» が画面を占め、＋と−の対比が消える。少し寄せておくと
    ///   芯の色そのものがどちら側から来た筋かを示す。
    float  coreTint;

    /// 帯を流れる粒の数 (帯 1 本あたり)。0 で粒を出さない。
    ///
    /// WHY ノイズの pulse と別に持つか:
    ///   pulse は値ノイズなので «明るさが波打つ» だけで、粒として数えられない。
    ///   等間隔の粒を走らせると «線の上を電荷が流れている» と読める。
    ///   線そのものを粒子で描くより桁違いに安く、線と粒が絶対にずれない。
    float  beadDensity;
    /// 粒の鋭さ。大きいほど点に近づき、小さいと尾を引く。
    float  beadFalloff;
};

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Material/Effects/ArcNoise.hlsli"
#include "Platform/Backend.hlsli"

struct ArcPSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

ArcPSIn VSMain(VSInput v)
{
    ArcPSIn o;
    float4 worldPos = mul(float4(v.position, 1.0f), world);
    o.svPosition = mul(worldPos, viewProjection);
    o.uv = v.uv;
    return o;
}

float4 PSMain(ArcPSIn input) : SV_Target0
{
    const float along  = saturate(input.uv.x);
    // 帯の中心を 0、両縁を 1 にした横断座標。断面の形はすべてこの値から作る。
    const float across = saturate(abs(input.uv.y - 0.5f) * 2.0f);

    // ── 筋の太さのムラ ───────────────────────────────────────────────────
    // 位相をずらしたノイズで芯を痩せさせる。太さを揺らすと、同じ折れ線でも
    // 毎フレーム違う放電に見える。
    const float grain     = ArcFbm(along * 14.0f + phase * 3.7f);
    const float thickness = lerp(1.0f, grain, saturate(breakup));

    // ── 断面: 白熱する芯 + 外へ広がるグロー ─────────────────────────────
    const float core = 1.0f - smoothstep(0.0f, max(coreWidth * thickness, 1.0e-4f), across);
    const float glow = pow(saturate(1.0f - across), max(glowFalloff, 0.01f));

    // ── 長手方向のフェード ───────────────────────────────────────────────
    // 両端は電極の内側へ潜り込ませたいので必ず絞る。絞らないと帯の切り口が
    // 四角いまま残り、電極から板が生えているように見える。
    const float ends  = smoothstep(0.0f, 0.08f, along) * smoothstep(0.0f, 0.08f, 1.0f - along);
    // erode は走り始め / 消え際の演出。中央から外へ向かって消していく。
    const float alive = 1.0f - smoothstep(1.0f - saturate(erode), 1.0f,
                                          abs(along - 0.5f) * 2.0f);

    // ── 帯に沿って流れる輝点 ─────────────────────────────────────────────
    // 放電に向きを与える。これが無いと、どちらの極からどちらへ流れているのか読めない。
    // 符号は流れる向き。負でも «流れている» ことに変わりはないので絶対値で判定する。
    const float pulse = abs(travel) > 0.0f
        ? pow(saturate(ArcNoise(along * 6.0f - phase * travel)), 3.0f)
        : 0.0f;

    // ── 帯を流れる粒 ─────────────────────────────────────────────────────
    // frac の鋸波を折り返して «粒の中心で 0 になる距離» にする。等間隔なので
    // 数えられる粒に見え、線の上を電荷が流れているように読める。
    float bead = 0.0f;
    if (beadDensity > 0.0f) {
        const float lane     = along * beadDensity - phase * travel;
        const float distance = abs(frac(lane) - 0.5f) * 2.0f;
        bead = pow(saturate(1.0f - distance), max(beadFalloff, 1.0f));
    }
    // 断面にも乗せる。帯いっぱいに光らせると «帯が明滅している» になって粒に見えない。
    const float beadShape = bead * pow(saturate(1.0f - across), 1.6f);

    // ── 配色: ＋極色 → −極色。芯だけは白熱させて極性から独立させる ───────
    const float3 rail = lerp(albedo.rgb, tipColor.rgb, along);
    // 芯はレール色へ寄せた白熱。掛け算で寄せるので、極性色の比率がそのまま残る。
    const float3 hotRgb = lerp(coreColor.rgb, coreColor.rgb * rail, saturate(coreTint));
    float3 color = rail * glow + hotRgb * core;
    color += rail * pulse * 1.5f;
    // 粒は «少しだけ» 芯の白熱へ寄せる。完全にレール色だと線に埋もれ、完全に白だと
    // 粒だけ極性から浮く。
    color += lerp(rail, hotRgb, 0.35f) * beadShape * 2.2f;
    color *= max(intensity, 0.0f);

    // 芯は不透明に寄せ、縁だけをアルファで溶かす。
    float alpha = saturate(glow * 0.85f + core + beadShape * 0.9f);
    alpha *= ends * alive * albedo.a;
    // 太さのムラは明るさよりアルファに強く効かせる。明るさだけを落とすと
    // 筋が細くならずに «灰色の帯» になる。
    alpha *= lerp(1.0f, thickness, saturate(breakup) * 0.75f);

    clip(alpha - 0.002f);
    return float4(color, alpha);
}
