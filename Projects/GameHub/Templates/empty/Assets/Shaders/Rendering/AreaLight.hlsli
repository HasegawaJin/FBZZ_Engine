/// @file AreaLight.hlsli
/// @brief 矩形の面光源 (Type::Area) の評価
/// @author Hasegawa Jin
/// @date 2026-08-25
//
// 拡散と鏡面で別の方法を採る。理由はそれぞれの「正解」の作り方が違うため。
//
//   拡散: LTC (Linearly Transformed Cosines) の拡散ケース。
//         拡散ローブはコサイン分布そのものなので変換行列が単位行列になり、
//         「多角形をコサインで積分する」閉じた式だけで厳密解が出る。LUT も要らない。
//
//   鏡面: representative point (UE4 方式)。反射レイと矩形の交点 (外れたら最近点) を
//         点光源とみなし、光源の見かけの大きさぶん roughness を広げて通常の GGX を引く。
//
// WHY 鏡面も LTC にしないか:
//   鏡面ローブ (GGX) を線形変換コサインへ写す行列は解析的に求まらず、
//   (roughness, N・V) の格子ごとに非線形最適化で当てた 64x64 のテーブルが要る。
//   あれはオフラインのフィッティングで作るもので、エンジンの起動時にも
//   コンピュートシェーダーでも生成できない。表を持ち込むまでは
//   representative point の方が「実装できて破綻しない」解になる。
//   ハイライトが矩形に沿って伸びるという面光源の一番の見どころは、こちらでも出る。
#ifndef AREA_LIGHT_HLSLI
#define AREA_LIGHT_HLSLI

#include "Common/ClusterConstants.hlsli"

// 1 / (2π)。Math.hlsli には無く、IBL/IBLCommon.hlsli が同名の static const を
// 持っているため、include 順によらず衝突しない専用の名前で置く。
static const float FBZZ_AREA_INV_TWO_PI = 0.15915494309189f;

// FBZZ_AreaEdgeIntegral — 球面上の辺 (v1 → v2) がコサイン分布へ与える寄与。
// LTC 論文の IntegrateEdge。接空間 (N = +Z) で評価するので z 成分だけ使う。
float FBZZ_AreaEdgeIntegral(float3 v1, float3 v2)
{
    const float cosTheta = clamp(dot(v1, v2), -1.0f, 1.0f);
    const float theta    = acos(cosTheta);
    const float sinTheta = sqrt(max(1.0f - cosTheta * cosTheta, 1e-7f));
    // theta / sin(theta) は theta → 0 で 1 に収束する。0 除算を避けて直接 1 を返す。
    const float ratio = (theta > 1e-5f) ? (theta / sinTheta) : 1.0f;
    return cross(v1, v2).z * ratio;
}

// FBZZ_AreaFormFactor — 矩形が受光点の半球へ張るコサイン重みつき立体角 [0,1]。
//   worldPos : 受光点
//   N        : 受光点の法線 (正規化済み)
//   p0..p3   : 矩形の 4 頂点 (ワールド空間・一巡する順)
//
// WHY 符号を abs で潰すか: 巻き方向は CPU が組み立てる tangent / bitangent の
//     向きで決まり、Transform のスケールが負なら反転する。表裏の判定は
//     呼び出し側が法線との内積で明示的に行うので、ここでは大きさだけ要る。
float FBZZ_AreaFormFactor(float3 worldPos, float3 N,
                          float3 p0, float3 p1, float3 p2, float3 p3)
{
    // 接空間 (N = +Z) を作る。V を使わないのは拡散ローブが視線に依存しないため。
    float3 T1 = p0 - worldPos;
    T1 = T1 - N * dot(T1, N);
    // 4 頂点すべてが法線方向に並ぶ退化配置では基底が作れない。寄与 0 で抜ける。
    const float t1Len = length(T1);
    if (t1Len < 1e-6f) return 0.0f;
    T1 /= t1Len;
    const float3 T2 = cross(N, T1);

    const float3x3 toTangent = float3x3(T1, T2, N);

    const float3 l0 = normalize(mul(toTangent, p0 - worldPos));
    const float3 l1 = normalize(mul(toTangent, p1 - worldPos));
    const float3 l2 = normalize(mul(toTangent, p2 - worldPos));
    const float3 l3 = normalize(mul(toTangent, p3 - worldPos));

    // 地平線でのクリップは行わず、積分してから絶対値を取る近似 (Hill の推奨)。
    // 頂点ごとにクリップすると分岐が増えるうえ、面が地平線をまたぐ瞬間に
    // 不連続が乗る。この近似は誤差が小さく、連続。
    float sum = FBZZ_AreaEdgeIntegral(l0, l1)
              + FBZZ_AreaEdgeIntegral(l1, l2)
              + FBZZ_AreaEdgeIntegral(l2, l3)
              + FBZZ_AreaEdgeIntegral(l3, l0);

    // 1/(2π) で正規化すると、半球を埋め尽くす面が 1.0 を返す単位になる。
    return saturate(abs(sum) * FBZZ_AREA_INV_TWO_PI);
}

// FBZZ_AreaRepresentativePoint — 鏡面用の「代表点」。
// 反射レイが矩形の面と交わる点を面内へクランプして返す。交わらない (背面へ向かう)
// 場合は矩形の中心へ落とす。
float3 FBZZ_AreaRepresentativePoint(PunctualLight lt, float3 worldPos, float3 N, float3 V)
{
    const float3 R = reflect(-V, N);

    const float denom = dot(R, lt.direction);
    float3 hit = lt.position;
    // 反射レイが面と平行に近いときは交点が無限遠へ飛ぶ。中心のままにする。
    if (abs(denom) > 1e-4f) {
        const float t = dot(lt.position - worldPos, lt.direction) / denom;
        if (t > 0.0f) hit = worldPos + R * t;
    }

    // 面内座標へ落として矩形の内側へクランプする。
    const float3 offset = hit - lt.position;
    const float2 local  = float2(
        clamp(dot(offset, lt.tangent),   -lt.halfWidth,  lt.halfWidth),
        clamp(dot(offset, lt.bitangent), -lt.halfHeight, lt.halfHeight));

    return lt.position + lt.tangent * local.x + lt.bitangent * local.y;
}

// FBZZ_EvalAreaLight — PunctualSample を面光源として組み立てる。
//
// 呼び出し側 (Lighting_*_Direct) は「1 本の L と 1 つの intensity」しか受け取れない。
// そこへ面光源を通すため、次の割り切りをしている:
//   L         … 鏡面の代表点への方向。ハイライトが矩形に沿って伸びる見た目はここで決まる。
//   intensity … 拡散の厳密な形態係数。ただし呼び出し側が改めて N・L を掛けるので、
//               二重計上にならないよう先に割っておく。
//
// WHY N・L で割るのに下限を置くか: 形態係数は既にコサインを織り込んでいるが、
//     面が広いと N・L → 0 でも係数は 0 に落ちない。素直に割ると грaze 角で発散するため、
//     0.05 (約 87 度) で頭打ちにする。そこから先は面光源が視野の縁に来ている状態で、
//     寄与そのものが小さいので打ち切りは目に見えない。
PunctualSample FBZZ_EvalAreaLight(PunctualLight lt, float3 worldPos, float3 N, float3 V)
{
    PunctualSample s;
    s.color         = lt.color;
    s.intensity     = 0.0f;
    s.L             = N;
    s.roughnessBias = 0.0f;
    s.intensityNoCosine = 0.0f;

    // 片面ライトは面の裏側を照らさない。
    // WHY 積分の符号で判定しないか: 巻き方向は CPU 側の基底の向き次第で反転しうる。
    //     「受光点が面の表側にあるか」を法線との内積で直接見る方が規約に依存しない。
    //     lt.outerCos は Area では両面フラグの運搬に使っている (1 = 両面)。
    const float3 toSurface = worldPos - lt.position;
    if (lt.outerCos < 0.5f && dot(lt.direction, toSurface) <= 0.0f)
        return s;

    // range による打ち切り。逆二乗はここでは掛けない。
    // WHY: 距離による減衰は形態係数 (立体角) が既に持っている。1/d^2 を重ねると
    //      面光源が点光源の 2 乗で暗くなり、近づくほど破綻する。
    const float dist = length(toSurface);
    const float t    = saturate(dist / max(lt.range, 1e-4f));
    const float t2   = t * t;
    float window = saturate(1.0f - t2 * t2);
    window *= window;
    if (window <= 0.0f) return s;

    const float3 halfT = lt.tangent   * lt.halfWidth;
    const float3 halfB = lt.bitangent * lt.halfHeight;
    const float3 p0 = lt.position - halfT - halfB;
    const float3 p1 = lt.position + halfT - halfB;
    const float3 p2 = lt.position + halfT + halfB;
    const float3 p3 = lt.position - halfT + halfB;

    const float formFactor = FBZZ_AreaFormFactor(worldPos, N, p0, p1, p2, p3);
    if (formFactor <= 0.0f) return s;

    const float3 rep = FBZZ_AreaRepresentativePoint(lt, worldPos, N, V);
    s.L = SafeNormalize(rep - worldPos, N);

    const float NdotL = max(dot(N, s.L), 0.05f);
    s.intensity = lt.intensity * window * formFactor / NdotL;
    // 体積の標本のように dot(N, L) を掛けない呼び出し側は、割る前の値を使う。
    // 割った値をそのまま使うと最大 20 倍 (1/0.05) 明るくなる。
    s.intensityNoCosine = lt.intensity * window * formFactor;

    // 矩形の見かけの大きさぶん反射ローブを広げる。長辺で代表させる。
    // WHY 面の形を無視して 1 つの値にするか: roughness はスカラーなので、
    //     異方性の広がり (縦長の窓が縦長のハイライトを作る) は表現できない。
    //     長辺で取ると「大きな面ほど広くにじむ」という主要な効果は残る。
    const float repDist = max(length(rep - worldPos), 1e-3f);
    s.roughnessBias = saturate(max(lt.halfWidth, lt.halfHeight) / (2.0f * repDist));
    return s;
}

#endif // AREA_LIGHT_HLSLI
