/// @file    FieldGrid.hlsl
/// @brief   タイトル背景の磁場グリッド。地平へ消える床を、電極が引き寄せて歪ませる。
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// PSO: SOLID_NOCULL + OPAQUE + DEPTH_ON (.mat の double_sided / blend_mode が決める)
///
/// カメラを包む球 1 枚へ描く。床は面ではなく「視線と平面の交点」で作るので、
/// 球は画面を埋めるための代理でしかなく、半径も分割数も絵に効かない。
///
/// WHY 空パスを差し替えないか:
///   RenderSystem が握る skyShader は Skydome.hlsl でハードコードされていて、
///   シーンごとに差し替えられない。書き換えると Main のアリーナと
///   SkyCapture → IBL まで巻き込む。球なら Title のシーンにだけ置けて、
///   HDR へ描かれるのでブルームも他の発光物と同じ経路で乗る。
///
/// WHY 時間を cbuffer から取らないか:
///   time を持つ PostProcConstants (b6) はジオメトリ描画では束縛されない。
///   Effects/Beam.hlsl と同じ理由で、phase は外から渡す。
///
/// WHY 電極の位置を外から渡すか:
///   ＋極はカーソルへ追従して毎フレーム動く (ElectrodeRig)。歪みの中心は .mat の
///   静的な値では表せない。TitleFieldGridComponent が ElectrodeRig::All() を
///   写して electrode0..3 へ書き込む。
///
/// WHY 背景を無彩色に保つか:
///   企画書 12.2 の「赤 = ＋ / 青 = − / 緑 = プレイヤー」は全アセット共通の制約で、
///   背景が色を持つと盤面の «色 = 意味» が薄まる。色が乗るのは電極の接地点だけで、
///   その色は極性そのものなので制約と矛盾しない。
///
/// WHY 線を明るくしすぎないか:
///   Title.fzdata のブルーム閾値は 0.28。格子の輝度をそこへ寄せると画面全体が
///   にじみ、「光っているのは電極」が読めなくなる。光る役は電極に譲る。

#include "Common/Binding.hlsli"

#define FBZZ_MATERIAL_CONSTANTS
cbuffer MaterialConstants : register(CB_MATERIAL)
{
    /// 細線の色 (HDR)。
    float4 gridColor;
    /// 太線の色 (HDR)。
    float4 majorColor;
    /// 地平の靄 (HDR)。床の果てと空の下端が同じ色で溶けるよう、上下へ対称に乗る。
    float4 horizonColor;
    /// 天頂の色。ほぼ黒。
    float4 zenithColor;
    /// 右刀の接地色。BladeColors::kColorRight をスクリプトが送る。
    float4 plusColor;
    /// 左刀の接地色。BladeColors::kColorLeft をスクリプトが送る。
    float4 minusColor;

    /// 細線の間隔 [m]。
    float  cellSize;
    /// 太線は細線 N 本ごと。
    float  majorEvery;
    /// 細線の太さ [px]。距離ではなく画面のピクセルで決まる (GridMask の WHY を参照)。
    float  lineWidth;
    /// 太線の太さ [px]。
    float  majorWidth;

    /// 床の高さ [m]。
    float  groundHeight;
    /// 格子の減衰が始まる距離 [m]。
    float  fadeStart;
    /// 格子が消えきる距離 [m]。地平の手前で消しきること (GridMask の WHY を参照)。
    float  fadeEnd;
    /// 地平の靄の厚み。レイの仰角の sin が尺度なので、0.05 前後で薄い帯になる。
    float  horizonThickness;

    /// 歪みの効く半径 [m]。電極が床に接している (高さ 0) ときの値。
    float  warpRadius;
    /// 歪みの深さ。1 で「電極の真下でセルが半分に詰まる」程度。
    float  warpStrength;
    /// 電極が床から離れたときの広がり方 [m]。この高さで足跡の半径が 2 倍・深さが半分になる。
    ///
    /// WHY 高さで効かせるか:
    ///   ＋極はカーソル追従で画面の上下いっぱいに動く (ElectrodeRig)。床への
    ///   落とし込みが xz だけだと、マウスを縦へ振っても足跡がまったく動かない。
    ///   高さを「広く浅く」へ変換すると、持ち上げれば場が床から離れて散り、
    ///   下ろせば 1 点へ締まる。縦の動きが絵に出るうえ、光源としても素直な挙動になる。
    float  heightFalloff;
    /// 接地の光だまりの半径 [m]。
    float  poolRadius;

    /// 光だまりの強さ。
    float  poolStrength;
    /// 波紋の間隔 [m]。
    float  ringSpacing;
    /// 波紋が外へ広がる速さ [m/s]。
    float  ringSpeed;
    /// 波紋の鋭さ。大きいほど細い輪になる。
    float  ringSharpness;

    /// 波紋の強さ。
    float  ringStrength;
    /// 時間 [秒]。スクリプトが毎フレーム進める (ファイル冒頭の WHY を参照)。
    float  phase;
    /// 全体の明るさ (HDR)。
    float  intensity;
    /// バンディング止め。ほぼ黒の階調は 8bit 換算で 2〜3 段しかなく、空に縞が出る。
    float  dither;

    /// 電極 4 本。xyz = ワールド位置、w = 極性 (+1 = ＋ / -1 = − / 0 = 未使用)。
    ///
    /// WHY 配列にしないか:
    ///   MaterialConstants の変数は .mat の [params] と **名前** で突き合わせる。
    ///   float4 electrodes[4] は 1 つの名前へ 64 バイトが対応してしまい、
    ///   MaterialInstance::SetVector4 の「4 成分」という検証を通らない。
    float4 electrode0;
    float4 electrode1;
    float4 electrode2;
    float4 electrode3;
};

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"

struct FieldGridPSIn
{
    float4 svPosition : SV_POSITION;
    float3 worldPos   : TEXCOORD0;
};

FieldGridPSIn VSMain(VSInput v)
{
    FieldGridPSIn o;
    const float4 worldPos = mul(float4(v.position, 1.0f), world);
    o.svPosition = mul(worldPos, viewProjection);
    o.worldPos   = worldPos.xyz;
    return o;
}

/// 電極が床へ落とす足跡の広がり。1 = 接地、大きいほど広く浅い。
float FootprintSpread(float4 electrode)
{
    const float height = max(electrode.y - groundHeight, 0.0f);
    return 1.0f + height / max(heightFalloff, 0.01f);
}

/// 電極 1 本ぶんの格子のずれ。＋は詰め、−は広げる。符号は w がそのまま持つ。
///
/// WHY normalize を使わないか:
///   方向ベクトルを正規化すると、電極の真下 (r → 0) で向きが定まらず格子が回る。
///   減衰した相似変形 (d を係数倍する) なら中心で 0 に収束するので特異点が出ない。
float2 WarpOffset(float2 p, float4 electrode)
{
    const float2 d      = p - electrode.xz;
    const float  spread = FootprintSpread(electrode);
    const float  radius = max(warpRadius, 0.01f) * spread;
    const float  amount = electrode.w * warpStrength / spread;
    // 下限を置かないと、−側の中心で係数が -1 を割って座標が裏返り、
    // 格子が «反転した島» になる。
    const float  k = max(amount / (1.0f + dot(d, d) / (radius * radius)), -0.9f);
    return d * k;
}

/// アンチエイリアス付きの格子。
///
/// WHY 距離ではなく fwidth で太さを決めるか:
///   歪ませたあとの座標は解析的な距離を持たない。隣の画素との差を使えば、
///   歪みも遠近もまとめて「画面上でどれだけ密か」になる。
///
/// WHY 密度で消すか:
///   1 画素が 1 セルを超えると格子は原理的に解像できず、線が潰れて一様な灰色の帯になる。
///   距離減衰 (fadeEnd) だけに任せると、カメラを寝かせた瞬間に地平へ灰色の板が出る。
float GridMask(float2 coord, float cell, float widthPixels)
{
    const float2 g    = coord / max(cell, 0.01f);
    const float2 grad = max(fwidth(g), 1.0e-5f);
    // セル境界を 0 に折り返した距離を、画素数へ直す。
    const float2 distPixels = abs(frac(g - 0.5f) - 0.5f) / grad;
    const float  nearest    = min(distPixels.x, distPixels.y);
    // NOTE: line はジオメトリシェーダーのプリミティブ指定子と衝突するので名前に使わない。
    const float  stroke     = 1.0f - smoothstep(0.0f, max(widthPixels, 0.5f), nearest);
    const float  resolve    = 1.0f - smoothstep(0.25f, 0.9f, max(grad.x, grad.y));
    return stroke * resolve;
}

/// 電極が床へ落とす光。だまりと、そこから広がる波紋の 2 つ。
///
/// WHY 床を光らせるか:
///   電極は宙に浮いていて、歪みの中心が «どの電極のものか» が絵から読めない。
///   接地点に光を落とすと、歪み・光だまり・電極が 1 本の線で結び付く。
float3 GroundLight(float2 p, float4 electrode)
{
    const float3 tint   = lerp(minusColor.rgb, plusColor.rgb, step(0.0f, electrode.w));
    const float  active = saturate(abs(electrode.w));   // w = 0 のスロットは何も出さない
    const float  spread = FootprintSpread(electrode);
    const float  radius = max(poolRadius, 0.01f) * spread;
    const float  r      = length(p - electrode.xz);

    const float pool = exp(-(r * r) / (radius * radius)) * poolStrength / spread;

    // frac の鋸波を折り返して «輪の中心で 0 になる距離» にする。等間隔なので
    // 数えられる輪に見え、外へ広がっていることが読める。
    const float lane = (r - phase * ringSpeed) / max(ringSpacing, 0.01f);
    const float edge = abs(frac(lane) - 0.5f) * 2.0f;
    const float ring = pow(saturate(1.0f - edge), max(ringSharpness, 1.0f))
                     * exp(-r / (radius * 3.0f)) * ringStrength;

    return tint * ((pool + ring) * active);
}

float4 PSMain(FieldGridPSIn input) : SV_Target0
{
    const float3 ray = normalize(input.worldPos - cameraPos);

    // 地平の靄。上下対称なので、床の果てと空の下端が同じ色で溶ける。
    const float haze = exp(-abs(ray.y) / max(horizonThickness, 1.0e-4f));

    float3 color = zenithColor.rgb + horizonColor.rgb * haze;

    // 床 — 視線と平面の交点。面を張らないので無限に続き、頂点も遠クリップも要らない。
    //
    // WHY 分岐で囲まないか:
    //   GridMask は fwidth を使う。地平をまたぐ 2x2 の画素塊で片方だけが分岐へ
    //   入ると微分が未定義になり、地平のきわで線の太さが暴れる。全画素で解いて
    //   から mask で捨てる。上を向いたレイは距離を頭打ちにして数値を有限に保つ。
    const float  below = max(cameraPos.y - groundHeight, 0.0f);
    const float  limit = max(fadeEnd, 1.0f) * 2.0f;
    const float  dist  = min(below / max(-ray.y, 1.0e-4f), limit);
    const float2 hit   = cameraPos.xz + ray.xz * dist;

    const float2 warped = hit
                        + WarpOffset(hit, electrode0)
                        + WarpOffset(hit, electrode1)
                        + WarpOffset(hit, electrode2)
                        + WarpOffset(hit, electrode3);

    const float minor = GridMask(warped, cellSize, lineWidth);
    const float major = GridMask(warped, cellSize * max(majorEvery, 1.0f), majorWidth);
    // 太線は細線を置き換える。足すと交点だけ二重に明るくなり、格子が «網» に見える。
    float3 floorColor = gridColor.rgb * minor * (1.0f - major) + majorColor.rgb * major;

    // 光だまりと波紋は歪ませる前の位置に置く。歪みは格子の話で、電極が居るのは実際の接地点。
    floorColor += GroundLight(hit, electrode0)
                + GroundLight(hit, electrode1)
                + GroundLight(hit, electrode2)
                + GroundLight(hit, electrode3);

    const float fade    = 1.0f - smoothstep(fadeStart, max(fadeEnd, fadeStart + 1.0f), dist);
    // 地平のすぐ下から立ち上げる。カメラが床と同じ高さのときは床が見えない。
    const float facing  = saturate(-ray.y * 1000.0f);
    const float onFloor = facing * step(1.0e-3f, below) * fade;

    color = (color + floorColor * onFloor) * intensity;

    const float noise = frac(sin(dot(input.svPosition.xy, float2(12.9898f, 78.233f))) * 43758.5453f);
    color += (noise - 0.5f) * dither;

    return float4(max(color, 0.0f), 1.0f);
}
