/// @file SpecularAA.hlsli
/// @brief 法線の画面内分散を roughness へ畳み込む幾何スペキュラアンチエイリアス
/// @author Hasegawa Jin
/// @date 2026-08-25
#ifndef SPECULAR_AA_HLSLI
#define SPECULAR_AA_HLSLI

// Tokuyoshi & Kaplanyan, "Improved Geometric Specular Antialiasing" (I3D 2019) の実用版。
//
// 1 ピクセルが覆う範囲で法線がどれだけ振れているかを ddx/ddy から測り、その分散を
// GGX の分布幅へ足し込む。ミップマップがテクスチャに対してやることを、法線に対して
// 毎フレームやる。ジオメトリが細かくなるほど (= 遠景・細いエッジ・高密度メッシュ)
// 1 ピクセル内の法線分布が広がるのに、GGX のローブ幅はマテリアルの roughness で
// 固定されたままになる。この差がハイライトの明滅として出る。
//
// WHY roughness を上げる方向にしか動かさないか:
//   分散の加算はローブを広げる操作で、狭める逆操作は存在しない。下げる方向へ動かすと
//   平坦面のハイライトまで鋭くなり、マテリアルが指定した艶とは別物になる。
//
// WHY PS 専用か:
//   ddx/ddy はピクセルクアッドの隣接ラップを読む。HLSL コンパイラはエントリポイントから
//   到達できない関数本体も検証するため、CS から include されるヘッダー
//   (Lighting.hlsli 等) へこの関数を置くとコンパイルが落ちる。

// カーネル分散。ピクセルフットプリントをガウシアンと見なしたときの重み。
static const float FBZZ_SPEC_AA_VARIANCE = 0.15f;

// 持ち上げ量の上限 (GGX の alpha^2 空間)。青天井にすると法線が大きく折れる輪郭で
// 一気にマット化し、シルエットに沿って帯が出る。
static const float FBZZ_SPEC_AA_THRESHOLD = 0.25f;

/// シェーディング法線の画面内微分から roughness を持ち上げる。ピクセルシェーダー専用。
///   shadingNormal      : 法線マップ適用後のワールド法線 (正規化済み)
///   perceptualRoughness: マテリアルの粗さ [0, 1] (GGX の alpha = この値の 2 乗)
/// 平坦な面では入力をそのまま返す。
float FilterSpecularRoughness(float3 shadingNormal, float perceptualRoughness)
{
    const float3 dndx = ddx(shadingNormal);
    const float3 dndy = ddy(shadingNormal);
    const float  variance = FBZZ_SPEC_AA_VARIANCE * (dot(dndx, dndx) + dot(dndy, dndy));

    const float alpha  = perceptualRoughness * perceptualRoughness;
    const float kernel = min(2.0f * variance, FBZZ_SPEC_AA_THRESHOLD);
    const float alphaSq = saturate(alpha * alpha + kernel);

    // alphaSq -> alpha -> perceptual の 2 段逆変換。
    return sqrt(sqrt(alphaSq));
}

#endif // SPECULAR_AA_HLSLI
