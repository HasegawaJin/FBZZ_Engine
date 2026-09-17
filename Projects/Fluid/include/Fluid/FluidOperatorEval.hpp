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

#include <Fluid/FluidRecipe.hpp>
#include <Fluid/FluidSourceMask.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::fluid {

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

/// 発生源の形の中での注入の重み [0,1] (形の外は 0。ノイズ揺らぎは含まない)。
/// minSize: これより小さい寸法はこの値まで広げる (格子ならセル幅)。d = p − center (2D は d.z = 0)、
/// a = normalize(direction) (長さ 0 なら (0,1,0))。
///   Sphere r=max(size.x,minSize): q²=|d|²/r²、w=(1−q²)²。Box h=max(size,minSize)各軸: q=max(|d|/h)、w=saturate((1−q)×4)
///   Cone L,R=size.y,size.x: t=dot(d,a)、[0,L]外は0、ρ=|d−at|、rt=max(Rt/L,minSize/2)、w=(1−ρ²/rt²)²×saturate((1−t/L)×4)
///   Ring R,r=size.x,size.y: h=dot(d,a)、ρ=|d−ah|−R、w=(1−(ρ²+h²)/r²)²
///   Capsule/Cylinder R,H=size.x,size.y: hc=clamp(dot(d,a),±H) (Cylinder は |h|>H で 0)、ρ=|d−ahc|、w=(1−ρ²/R²)²
/// いずれも境界で q² (Box は q) >= 1 なら 0。
[[nodiscard]] float FluidSourceWeight(const FluidSource& source, const math::Vector3& center, const math::Vector3& p,
                                      float minSize, bool volumetric);

/// 液体の粒子を湧かせる位置: 形の中でほぼ一様な点。u0..u2 は [0,1) の乱数。2D では z = center.z の
/// 断面から選ぶ。Texture は板 (箱) の中の一様な点 (マスクは見ない — 下の関数を使う)。
/// Capsule / Cylinder の 3D は体積一様: Cylinder は h=H(2u0−1)、r=R√u1、φ=2πu2 の円柱座標。
/// Capsule は円柱と両端半球を体積比で選び分け、半球側は球内の一様点を軸端 (±aH) へ平行移動する。
/// 2D は断面の軸方向 α に一様、直交方向 β を α での半幅に一様に選ぶ (端がわずかに濃くなる —
/// SampleRing2D と同じ流儀)。
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

/// 液体の発生源が詰まりすぎるのを避けるために内部で広げる倍率 (1 = そのまま広げない)。
/// 同時に発生源の中に居る粒が静止密度の 2 倍までで収まる大きさへ、形を中心から相似に拡大する。
/// @note 公開する理由: 広げたことが UI に出ないと、Inspector の半径 0.06 と実際に湧く大きさが食い違って見える。
[[nodiscard]] float FluidLiquidEmitScale(const FluidSource& source, const FluidLiquidSettings& liquid,
                                         bool volumetric);

/// 障害物の time での姿・居るか (発生源と同じ規則)。
[[nodiscard]] FluidOperatorPose PoseFluidCollider(const FluidCollider& collider, float time);
[[nodiscard]] bool FluidColliderActive(const FluidCollider& collider, float time);

/// 障害物への符号付き距離 (外が正・中が負)。d = p − center (2D は d.z = 0)。気体の格子はセル中心の
/// 距離が 0 未満のセルを固体とする。
///   Sphere: |d| − max(size.x, minSize)。Box h=max(size,minSize)各軸: q=|d|−h、|max(q,0)|+min(max(q),0)
///   Plane : dot(d,n) (n=normalize(direction)、長さ0なら(0,1,0))。法線側が外
///   Capsule R,H=size.x,size.y、a=normalize(direction): hc=clamp(dot(d,a),±H)、|d−a hc|−R
///   Cylinder: 同じ R・H・a で h=dot(d,a)、ρ=|d−ah|、dr=ρ−R、dh=|h|−H、|max((dr,dh),0)|+min(max(dr,dh),0)
[[nodiscard]] float FluidColliderDistance(const FluidCollider& collider, const math::Vector3& center,
                                          const math::Vector3& p, float minSize, bool volumetric);

/// 表面の外向き単位法線 (距離の勾配)。2D では z を 0 にして正規化し直す (長さ 0 なら (0,1,0))。
///   Sphere: d/|d| (|d|<1e-6 なら (0,1,0))。Box: 外なら max(q,0)×sign(d) を正規化、中なら q 最大軸の sign(d)
///   Plane : n
///   Capsule: (d−a hc)/|d−a hc|。芯の上 (長さ<1e-6) は e1=normalize(cross(a, |a.x|<0.9?(1,0,0):(0,1,0)))
///           (決まった手順で 1 つ選ぶ。up を返すと軸が上向きのとき芯に沿って押してしまう)
///   Cylinder: u=(d−ah)/|d−ah| (芯の上なら e1)。外なら u max(dr,0)+a sign(h) max(dh,0) を正規化、
///           中なら dr>=dh で u、そうでなければ a sign(h) (Box の同じ規則)
[[nodiscard]] math::Vector3 FluidColliderNormal(const FluidCollider& collider, const math::Vector3& center,
                                                const math::Vector3& p, float minSize, bool volumetric);

/// @brief 力が dt 秒で与える速度の変化 Δv。forceIndex は有効な部品だけを数えた添字 (GPU も同じ順で数える)。
/// @note 呼び出し順: 浮力・全体風・乱流の後、減衰の前に足す (Drag は途中まで足した速度を見る)。
/// @note 強さ→influence の順で掛ける: GPU は定数へ strength×倍率を入れシェーダーが influence を掛けるため、
///       同じ順でないと丸めの分だけ CPU と GPU の絵がずれる。
///   d=p−center (2D はd.z=0)、influence=radius>0?(1−dist/radius)^falloffPower:1 (dist>=radius なら0)、
///   s=strength×strengthScale×influence、a=normalize(direction) (長さ0なら(1,0,0))。2D は Δv.z=0。
///   Wind:a×s×dt  Attract/Repulse:∓(d/dist)×s×dt (dist<1e-5で0)  Vortex:(cross(k,d)正規化)×s×dt
///   Noise:CurlNoise(p×freq+noiseOffset+idx依存オフセット)×s×dt  Drag:−velocity×(1−exp(−max(s,0)×dt))
[[nodiscard]] math::Vector3 FluidForceDelta(const FluidForce& force, const math::Vector3& center,
                                            const math::Vector3& p, const math::Vector3& velocity, float time,
                                            float dt, const math::Vector3& noiseOffset, int forceIndex,
                                            bool volumetric, float strengthScale);

} // namespace fbzz::fluid
