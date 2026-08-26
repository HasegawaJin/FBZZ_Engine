/// @file SphereTubeLight.hlsli
/// @brief 球 / カプセル形状を持つ光源の評価 (representative point 法)
/// @author Hasegawa Jin
/// @date 2026-08-25
//
// Karis 2013 "Real Shading in Unreal Engine 4" の representative point。
// 反射ローブの中心線 (反射レイ) に最も近い光源上の点を選んで点光源として扱う。
//
// WHY 代表点だけでは足りず正規化が要るか:
//   代表点は「一番強く反射する場所」なので、そのまま点光源として評価すると
//   光源が大きいほど明るくなってしまう (本来は同じ総量が広がるだけ)。
//   GGX の正規化項が roughness に依存することを使い、光源の見かけの大きさぶん
//   roughness を広げたときの正規化比を掛け戻してエネルギーを保つ。
#ifndef SPHERE_TUBE_LIGHT_HLSLI
#define SPHERE_TUBE_LIGHT_HLSLI

#include "Common/ClusterConstants.hlsli"

// FBZZ_ClosestPointOnSegment — 線分 (center ± axis*halfLength) 上で p に最も近い点。
float3 FBZZ_ClosestPointOnSegment(float3 center, float3 axis, float halfLength, float3 p)
{
    const float t = clamp(dot(p - center, axis), -halfLength, halfLength);
    return center + axis * t;
}

// FBZZ_ClosestPointOnRay — レイ (origin + dir*t, t >= 0) 上で p に最も近い点。
float3 FBZZ_ClosestPointOnRay(float3 origin, float3 dir, float3 p)
{
    return origin + dir * max(dot(p - origin, dir), 0.0f);
}

// FBZZ_SphereRepresentativePoint — 反射レイに最も近い球面上の点。
// 球の中心からレイへの垂線の足を求め、そこへ半径ぶん寄せる。
float3 FBZZ_SphereRepresentativePoint(float3 worldPos, float3 R,
                                      float3 center, float radius)
{
    const float3 toCenter   = center - worldPos;
    const float3 centerOnRay = R * max(dot(toCenter, R), 0.0f);
    const float3 offset      = centerOnRay - toCenter;
    const float  offsetLen   = length(offset);
    // レイが球を貫いているときは中心のままでよい (代表点は中心が最良)。
    if (offsetLen <= radius) return center;
    return center + offset * (radius / offsetLen);
}

// FBZZ_SourceRadiusRoughnessBias — 光源の大きさが反射ローブを広げる量。
//   radius : 光源の半径 [m]
//   dist   : 受光点から代表点までの距離 [m]
//
// 見かけの半角 ≈ radius / (2*dist) をそのまま roughness へ足す量として返す。
//
// WHY intensity ではなく roughness を動かすか: 大きな光源は「明るくなる」のではなく
//     「同じ総量が広い範囲へ散る」。intensity を下げて辻褄を合わせると、同じ係数が
//     拡散にも掛かってしまい、半径を上げるほど面全体が暗くなるという逆の破綻を生む。
//     ローブを広げるのが物理的に正しい対処で、GGX の正規化がエネルギーを保存する。
float FBZZ_SourceRadiusRoughnessBias(float radius, float dist)
{
    if (radius <= 0.0f) return 0.0f;
    return saturate(radius / max(2.0f * dist, 1e-4f));
}

// FBZZ_EvalSphereTubeLight — 球 / カプセル光源を PunctualSample へ落とす。
//
// 呼び出し側 (Lighting_*_Direct) は 1 本の L と 1 つの intensity しか受け取れないので、
// 拡散と鏡面で別の点を使い分けることはできない。ここでは:
//   L         … 鏡面の代表点への方向。ハイライトが球なら丸く、カプセルなら線状に伸びる。
//   intensity … 拡散として正しい「形状の中心までの距離」で減衰させ、
//               鏡面の広がりぶんのエネルギー補正を掛けたもの。
//
// WHY 減衰に代表点までの距離を使わないか: 代表点は面のすぐ手前に寄るので、
//     そこまでの距離で 1/d^2 を取ると光源に近づいたとき拡散が発散する。
//     拡散の正解は形状の中心 (カプセルなら軸上の最近点) までの距離。
PunctualSample FBZZ_EvalSphereTubeLight(PunctualLight lt, float3 worldPos, float3 N, float3 V)
{
    PunctualSample s;
    s.color     = lt.color;
    s.intensity = 0.0f;
    s.L         = N;
    // 下の atten == 0 で打ち切る経路がここを通る。埋めておかないと未初期化のまま返り、
    // 呼び出し側の saturate(roughness + ps.roughnessBias) が不定値を読む。
    // (FBZZ_EvalAreaLight は最初から全部埋めている。合わせておく)
    s.roughnessBias     = 0.0f;
    s.intensityNoCosine = 0.0f;

    const float radius = max(lt.halfWidth, 0.0f);

    // 拡散の基準点。カプセルは軸上で最も近い点、球は中心。
    // WHY カプセルで軸上の最近点を使うか: 蛍光灯の真下と端では距離が全く違う。
    //     中心固定だと管の長さが伸びるほど真下が暗くなる。
    float3 diffuseOrigin = lt.position;
    if (lt.type == FBZZ_LIGHT_TYPE_TUBE) {
        diffuseOrigin = FBZZ_ClosestPointOnSegment(
            lt.position, lt.tangent, max(lt.halfHeight, 0.0f), worldPos);
    }

    const float3 toLight = diffuseOrigin - worldPos;
    // 表面が光源の内側に入ったときの発散を防ぐ。半径ぶんは必ず離れているとみなす。
    const float  dist    = max(length(toLight) - radius, 1e-3f);

    const float atten = LightAttenuation(dist, lt.range);
    if (atten <= 0.0f) return s;

    // 鏡面の代表点。カプセルは「反射レイに最も近い軸上の点」を球の中心とみなす。
    const float3 R = reflect(-V, N);
    float3 sphereCenter = lt.position;
    if (lt.type == FBZZ_LIGHT_TYPE_TUBE) {
        // 軸上の点とレイ上の点を交互に寄せる 1 回の反復。Karis の近似そのまま。
        const float3 rayPoint = FBZZ_ClosestPointOnRay(worldPos, R, lt.position);
        sphereCenter = FBZZ_ClosestPointOnSegment(
            lt.position, lt.tangent, max(lt.halfHeight, 0.0f), rayPoint);
    }

    float3 rep = sphereCenter;
    if (radius > 0.0f)
        rep = FBZZ_SphereRepresentativePoint(worldPos, R, sphereCenter, radius);

    s.L = SafeNormalize(rep - worldPos, N);

    // 拡散のコサインは「形状表面の最近点」への方向で取る。
    //
    // WHY s.L で取ってはいけないか: s.L は鏡面の代表点への方向で、反射レイ R から決まる
    //     = 視線に依存して光源の軸上を動く。呼び出し側は L を 1 本しか受け取れず
    //     dot(N, s.L) を掛けるので、そのままだと拡散まで視線依存になる。とくに長い管では
    //     反射レイが軸に沿った瞬間に代表点がセグメントの端へクランプされ、管の真下
    //     (本来いちばん明るい場所) でコサインが落ちて暗くなる。端だけ光って見える。
    // 呼び出し側が掛け直す dot(N, s.L) をここで割って打ち消す。
    //     FBZZ_EvalAreaLight が形態係数に対して行っているのと同じ処理で、下限 0.05 も同じ。
    // NOTE: 鏡面側は本来 dot(N, s.L) が正しいので、この補正は鏡面をわずかに歪める。
    //       L と intensity が 1 組しかない以上どちらかしか合わせられず、面積の広い拡散を
    //       正とする (Area と同じ判断)。
    const float diffuseNdotL = saturate(dot(N, SafeNormalize(toLight, N)));
    const float specNdotL    = max(dot(N, s.L), 0.05f);
    s.intensity = lt.intensity * atten * diffuseNdotL / specNdotL;
    // 体積の標本のように dot(N, L) を掛けない呼び出し側は、補正前の素の値を使う。
    s.intensityNoCosine = lt.intensity * atten;

    // 反射ローブの広がり。呼び出し側が roughness へ足して BRDF を評価する。
    const float repDist = max(length(rep - worldPos), 1e-3f);
    s.roughnessBias = FBZZ_SourceRadiusRoughnessBias(radius, repDist);
    return s;
}

#endif // SPHERE_TUBE_LIGHT_HLSLI
