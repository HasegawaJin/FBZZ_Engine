/// @file    FluidOperatorEval.hpp
/// @brief   流体レシピの部品 (発生源・力・動き) の評価式。CPU の全ソルバーが呼ぶ正本
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// GPU は Assets/Shaders/Bake/Fluid/FluidGpuCommon.hlsli に同じ式の写しを持つ (FluidSourceWeight /
/// FluidForceDelta)。動き (FluidMotion) と量 (FluidAmount) は GPU では評価しない — PackFluidGpuStep が
/// 刻みごとに CPU でここを呼び、«今の中心と速度» と «倍率を掛けた量・強さ» を定数へ詰める。
/// だからエンベロープを足してもシェーダーと定数バッファは変わらない。
///
/// 座標は領域座標 (最長軸 = [-1,1]、y 上向き)。2D (volumetric = false) では奥行きを見ない:
/// 位置の差の z を 0 として扱い、結果の z も 0 にする。
#pragma once

#include <Engine/Asset/FluidRecipe.hpp>
#include <Engine/Asset/FluidSourceMask.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::asset {

struct FluidMotionSample {
    math::Vector3 offset;
    /// キー間の傾き (区間の外・キーが 1 つ以下なら 0)。
    math::Vector3 velocity;
};

/// キーが無ければ 0。time が先頭以前・末尾以降なら端のキーの offset で、速度は 0。
/// キー間は直線補間し、速度は (後 − 前) / (時刻の差) (差が 1e-6 未満なら 0)。
[[nodiscard]] FluidMotionSample SampleFluidMotion(const FluidMotion& motion, float time);

/// 量の倍率。キーが無ければ 1。time が先頭以前・末尾以降なら端のキーの scale (外挿しない)。
/// キー間は直線補間する:
///   s = from.scale + (to.scale − from.scale) × (time − from.time) / (to.time − from.time)
/// (時刻の差が 1e-6 未満なら from.scale。)
[[nodiscard]] float SampleFluidAmount(const FluidAmount& amount, float time);

/// time における部品の «今の姿»。
struct FluidOperatorPose {
    /// 基準の center + 動きのずれ。
    math::Vector3 center;
    /// 動きの速度 (motion.inheritVelocity が false なら 0)。
    math::Vector3 motionVelocity;
};
[[nodiscard]] FluidOperatorPose PoseFluidSource(const FluidSource& source, float time);
[[nodiscard]] FluidOperatorPose PoseFluidForce(const FluidForce& force, float time);

/// time に発生源が注ぐ量の倍率 (= max(SampleFluidAmount(source.amount, time), 0))。
/// density / temperature / fuel に掛ける。位置と velocity には掛けない (動きは motion の担当)。
/// 液体はこの 3 つを使わないので効かない — 撒く量は count / duration が決める。
/// **0 で止める**: 負にすると質量を引くことになり、散逸・移流・色の鍵 (mass / max(carrier, ε)) と
/// 黒体放射 (T^4) が符号を想定していない。«煙を削る» は倍率の裏口ではなく専用の部品で設計する。
[[nodiscard]] float FluidSourceAmount(const FluidSource& source, float time);
/// time に力が出す強さの倍率 (= SampleFluidAmount(force.amount, time))。strength に掛ける
/// (FluidForceDelta の strengthScale に渡す値)。
/// こちらは**負を止めない** — strength が元から負を許す (2D 渦の逆回転) ので、倍率も符号を通す。
[[nodiscard]] float FluidForceAmount(const FluidForce& force, float time);

/// 気体の発生源が time に注いでいるか: enabled && time >= startTime && (duration <= 0 || time < startTime + duration)。
/// (液体の撃ち出しは数で決まるのでソルバーが数える。)
[[nodiscard]] bool FluidSourceEmitting(const FluidSource& source, float time);
/// 力が time に効いているか (FluidSourceEmitting と同じ規則)。
[[nodiscard]] bool FluidForceActive(const FluidForce& force, float time);

/// 発生源の形の中での注入の重み [0,1]。形の外は 0。ノイズによる揺らぎは含まない (ソルバー側で掛ける)。
/// minSize: これより小さい寸法はこの値まで広げる (格子ならセル幅。広げないと 1 セルにも入らない)。
/// d = p − center (2D は d.z = 0)。a = normalize(direction) (長さ 0 なら (0,1,0))。
///   Sphere: r = max(size.x, minSize)。q² = |d|²/r²。w = (1 − q²)²
///   Box   : h = max(size, minSize) (各軸)。q = max(|d.x|/h.x, |d.y|/h.y, |d.z|/h.z)。w = saturate((1 − q) × 4)
///   Cone  : L = max(size.y, minSize)、R = max(size.x, minSize)。t = dot(d, a)。t < 0 か t > L なら 0。
///           半径 ρ = |d − a t|、その高さでの許容半径 rt = max(R t / L, minSize × 0.5)。q² = ρ²/rt²。
///           w = (1 − q²)² × saturate((1 − t/L) × 4)   (底の縁でなめらかに 0)
///   Ring  : R = max(size.x, minSize)、r = max(size.y, minSize)。h = dot(d, a)、ρ = |d − a h| − R。
///           q² = (ρ² + h²)/r²。w = (1 − q²)²
///   Capsule : R = max(size.x, minSize)、H = max(size.y, minSize)。hc = clamp(dot(d, a), −H, H)。
///           ρ = |d − a hc| (= 芯の線分への距離)。q² = ρ²/R²。w = (1 − q²)²
///           (H = 0 なら芯が 1 点なので球と同じ形になる — 端が半球であることの裏付け)
///   Cylinder: R = max(size.x, minSize)、H = max(size.y, minSize)。h = dot(d, a)。|h| > H なら 0。
///           ρ = |d − a h| (= 芯の «直線» への距離)。q² = ρ²/R²。
///           w = (1 − q²)² × saturate((1 − |h|/H) × 4)   (両端でなめらかに 0。Cone の底と同じ流儀)
/// いずれも q² >= 1 (Box は q >= 1) なら 0。
[[nodiscard]] float FluidSourceWeight(const FluidSource& source, const math::Vector3& center, const math::Vector3& p,
                                      float minSize, bool volumetric);

/// 液体の粒子を湧かせる位置: 形の中でほぼ一様な点。u0..u2 は [0,1) の乱数。
/// 2D では z = center.z の断面から選ぶ。Texture は板 (箱) の中の一様な点 (マスクは見ない — 下の関数を使う)。
///
/// Capsule / Cylinder の 3D は体積一様:
///   Cylinder: h = H(2u0 − 1)、r = R√u1、φ = 2π u2。offset = a h + e1 r cosφ + e2 r sinφ
///   Capsule : 円柱 (体積 2πR²H) と両端の半球 (合わせて球 1 つ = 4/3 πR³) を体積比で選び分け、
///             半球側は球の中の一様な点を取って、軸方向の符号の側の端 (±a H) へ平行移動する
/// 2D は z = center.z の断面 («面内の軸方向 α» と «それに直交する β») から取る。α を全長に一様、
/// β をその α での半幅に一様に選ぶ (必ず形の中に入るが、細くなる端がわずかに濃くなる — SampleRing2D と同じ流儀)。
[[nodiscard]] math::Vector3 SampleFluidSourcePoint(const FluidSource& source, const math::Vector3& center,
                                                   float u0, float u1, float u2, bool volumetric);

/// Texture の板の軸。n = normalize(direction) (長さ 0 なら (0,0,1) = 手前向き)。
/// |n.y| < 0.99 なら right = normalize(cross((0,1,0), n))、そうでなければ right = (1,0,0)。up = cross(n, right)。
/// (n = (0,0,1) で right = (1,0,0)、up = (0,1,0) — 画像がそのまま正面に見える)
void FluidTextureSourceBasis(const FluidSource& source, math::Vector3& outRight, math::Vector3& outUp,
                             math::Vector3& outNormal);

/// Texture の注入の重み。d = p − center (2D は d.z = 0)、h = max(size, minSize) (各軸)。
/// a = dot(d, right)/h.x、b = dot(d, up)/h.y、c = dot(d, n)/h.z。|a|・|b|・|c| のどれかが 1 以上なら 0。
/// w = SampleFluidSourceMask(mask, (a + 1)/2, (1 − b)/2) × saturate((1 − |c|) × 4)。
/// FluidSourceWeight に Texture を渡した場合はマスク 1 (= 板の形) として扱う。
[[nodiscard]] float FluidTextureSourceWeight(const FluidSource& source, const math::Vector3& center,
                                             const math::Vector3& p, float minSize, bool volumetric,
                                             const FluidSourceMask& mask);

/// 液体: Texture の板の中の点を u0..u2 で選び、accept (一様乱数 [0,1)) がその点のマスク値以上なら false
/// (呼び手が引き直す。何度か外れたら諦めてよい)。受け入れたら outPoint に入れて true。2D は板の z = center.z 断面。
[[nodiscard]] bool SampleFluidTextureSourcePoint(const FluidSource& source, const math::Vector3& center,
                                                 const FluidSourceMask& mask, float u0, float u1, float u2,
                                                 float accept, bool volumetric, math::Vector3& outPoint);

/// 液体の発生源が «詰まりすぎ» を避けるために内部で広げる倍率 (1 = そのまま広げない)。
/// 同時に発生源の中に居る粒が静止密度の 2 倍までで収まる大きさへ、形を中心から相似に拡大する。
/// WHY 公開するか: 広げたことが UI に出ないと、Inspector の半径 0.06 と実際に湧く大きさが食い違って見える。
[[nodiscard]] float FluidLiquidEmitScale(const FluidSource& source, const FluidLiquidSettings& liquid,
                                         bool volumetric);

/// 障害物の time での姿・居るか (発生源と同じ規則)。
[[nodiscard]] FluidOperatorPose PoseFluidCollider(const FluidCollider& collider, float time);
[[nodiscard]] bool FluidColliderActive(const FluidCollider& collider, float time);

/// 障害物への符号付き距離 (外が正・中が負)。d = p − center (2D は d.z = 0)。
///   Sphere: |d| − max(size.x, minSize)
///   Box   : h = max(size, minSize) (各軸)、q = |d| − h (各軸)。|max(q, 0)| + min(max(q.x, q.y, q.z), 0)
///   Plane : dot(d, n)。n = normalize(direction) (長さ 0 なら (0,1,0))。法線の側が外
///   Capsule : a = normalize(direction) (長さ 0 なら (0,1,0))、R = max(size.x, minSize)、H = max(size.y, minSize)。
///           hc = clamp(dot(d, a), −H, H)。|d − a hc| − R
///   Cylinder: 同じ R・H・a で h = dot(d, a)、ρ = |d − a h|。dr = ρ − R、dh = |h| − H。
///           |max((dr, dh), 0)| + min(max(dr, dh), 0)   (Box を «半径と軸» の 2 軸で測った形)
/// 気体の格子はセル中心の距離が 0 未満のセルを固体とする。
[[nodiscard]] float FluidColliderDistance(const FluidCollider& collider, const math::Vector3& center,
                                          const math::Vector3& p, float minSize, bool volumetric);

/// 表面の外向き単位法線 (距離の勾配)。
///   Sphere: d/|d| (|d| < 1e-6 なら (0,1,0))
///   Box   : 外 (距離 > 0) なら max(q, 0) × sign(d) を正規化、中なら q が最大の軸について sign(d) の単位軸
///   Plane : n
///   Capsule : (d − a hc)/|d − a hc|。長さが 1e-6 未満 (芯の上) なら軸に直交する既定の向き e1
///           (e1 = normalize(cross(a, |a.x| < 0.9 ? (1,0,0) : (0,1,0)))。芯の上は «どちら向きでもよい» ので
///            決まった手順で 1 つ選ぶ。up を返すと軸が上向きのとき芯に沿って押してしまう)
///   Cylinder: 半径の向き u = (d − a h)/|d − a h| (芯の上なら e1)、軸の向き ±a。
///           外 (max(dr, dh) > 0) なら u max(dr, 0) + a sign(h) max(dh, 0) を正規化、
///           中なら dr >= dh で u、そうでなければ a sign(h) (Box の «同じ深さなら先の軸» と同じ規則)
/// 2D では z を 0 にして正規化し直す (長さ 0 になったら (0,1,0))。
[[nodiscard]] math::Vector3 FluidColliderNormal(const FluidCollider& collider, const math::Vector3& center,
                                                const math::Vector3& p, float minSize, bool volumetric);

/// 力が dt 秒で与える速度の変化 Δv。forceIndex はノイズの切り出し位置をずらすのに使う
/// (有効な部品だけを数えた添字。GPU は有効な部品だけを詰めた順で数えるので、それに揃える)。
/// 力はリストの順に、浮力・全体の風・乱流の後、減衰の前に足す (Drag は途中まで足した速度を見る)。
/// time は刻みの開始時刻。
/// noiseOffset は seed から決まる格子の切り出し位置 (FluidNoiseOffset。液体は 0 でよい)。
/// strengthScale は量のエンベロープの倍率 (FluidForceAmount)。1 を渡せば掛けないのと同じ。
/// WHY 強さに掛けてから influence を掛けるか: GPU は詰める段で strength × 倍率 を定数へ入れ、
///     シェーダーが influence を掛ける。同じ順で掛けないと丸めの分だけ CPU と GPU の絵がずれる。
/// d = p − center (2D は d.z = 0)、dist = |d|。
/// strength = force.strength × strengthScale (0 なら Δv = 0)。
/// influence = radius > 0 ? (dist >= radius ? 0 : (1 − dist/radius)^falloffPower) : 1。
/// a = normalize(direction) (長さ 0 なら (1,0,0))、s = strength × influence。
///   Wind   : Δv = a × s × dt
///   Attract: Δv = −(d/dist) × s × dt        (dist < 1e-5 なら 0)
///   Repulse: Δv = +(d/dist) × s × dt        (dist < 1e-5 なら 0)
///   Vortex : 軸 k = volumetric ? a : (0,0,1)。t = cross(k, d)。|t| < 1e-5 なら 0。Δv = (t/|t|) × s × dt
///   Noise  : q = p × noiseFrequency + noiseOffset + (forceIndex × 17.31, −time × noiseSpeed, forceIndex × 5.73)。
///            Δv = CurlNoise(q) × s × dt     (core::CurlNoise / HLSL FluidCurlNoise)
///   Drag   : Δv = −velocity × (1 − exp(−max(s, 0) × dt))   (dt が大きくても振動しない。負の強さは効かない)
/// 2D では Δv.z = 0。
[[nodiscard]] math::Vector3 FluidForceDelta(const FluidForce& force, const math::Vector3& center,
                                            const math::Vector3& p, const math::Vector3& velocity, float time,
                                            float dt, const math::Vector3& noiseOffset, int forceIndex,
                                            bool volumetric, float strengthScale);

} // namespace fbzz::asset
