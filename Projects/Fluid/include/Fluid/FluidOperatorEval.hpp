/// @file    FluidOperatorEval.hpp
/// @brief   流体レシピの部品 (発生源・力・動き) の評価式。CPU の全ソルバーが呼ぶ正本
/// @author  Hasegawa Jin
/// @date    2026-09-12

/// @note GPU は Assets/Shaders/Bake/Fluid/FluidGpuCommon.hlsli に同じ式の写しを持つ (FluidSourceWeight /
/// @note FluidForceDelta)。動き (FluidMotion) と量 (FluidAmount) は GPU では評価しない — PackFluidGpuStep が
/// @note 刻みごとに CPU でここを呼び、«今の中心と速度» と «倍率を掛けた量・強さ» を定数へ詰める。
/// @note だからエンベロープを足してもシェーダーと定数バッファは変わらない。

/// @note 座標は領域座標 (最長軸 = [-1,1]、y 上向き)。2D (volumetric = false) では奥行きを見ない:
/// @note 位置の差の z を 0 として扱い、結果の z も 0 にする。
#pragma once
#include <Fluid/Api.hpp>

#include <Fluid/FluidRecipe.hpp>
#include <Fluid/FluidSourceMask.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::fluid {

struct FluidMotionSample {
    math::Vector3 offset;
    /// @note キー間の傾き (区間の外・キーが 1 つ以下なら 0)。
    math::Vector3 velocity;
};

/// @note キーが無ければ 0。time が先頭以前・末尾以降なら端のキーの offset で、速度は 0。
/// @note キー間は直線補間し、速度は (後 − 前) / (時刻の差) (差が 1e-6 未満なら 0)。
[[nodiscard]] FBZZ_FLUID_API FluidMotionSample SampleFluidMotion(const FluidMotion& motion, float time);

/// @note 量の倍率。キーが無ければ 1。time が先頭以前・末尾以降なら端のキーの scale (外挿しない)。
/// @note キー間は直線補間する:
/// @note s = from.scale + (to.scale − from.scale) × (time − from.time) / (to.time − from.time)
/// @note (時刻の差が 1e-6 未満なら from.scale。)
[[nodiscard]] FBZZ_FLUID_API float SampleFluidAmount(const FluidAmount& amount, float time);

/// @note time における部品の «今の姿»。
struct FluidOperatorPose {
    /// @note 基準の center + 動きのずれ。
    math::Vector3 center;
    /// @note 動きの速度 (motion.inheritVelocity が false なら 0)。
    math::Vector3 motionVelocity;
};
[[nodiscard]] FBZZ_FLUID_API FluidOperatorPose PoseFluidSource(const FluidSource& source, float time);
[[nodiscard]] FBZZ_FLUID_API FluidOperatorPose PoseFluidForce(const FluidForce& force, float time);

/// @note time に発生源が注ぐ量の倍率 (= max(SampleFluidAmount(source.amount, time), 0))。
/// @note density / temperature / fuel に掛ける。位置と velocity には掛けない (動きは motion の担当)。
/// @note 液体はこの 3 つを使わないので効かない — 撒く量は count / duration が決める。
/// @note **0 で止める**: 負にすると質量を引くことになり、散逸・移流・色の鍵 (mass / max(carrier, ε)) と
/// @note 黒体放射 (T^4) が符号を想定していない。«煙を削る» は倍率の裏口ではなく専用の部品で設計する。
[[nodiscard]] FBZZ_FLUID_API float FluidSourceAmount(const FluidSource& source, float time);
/// @note time に力が出す強さの倍率 (= SampleFluidAmount(force.amount, time))。strength に掛ける
/// @note (FluidForceDelta の strengthScale に渡す値)。
/// @note こちらは**負を止めない** — strength が元から負を許す (2D 渦の逆回転) ので、倍率も符号を通す。
[[nodiscard]] FBZZ_FLUID_API float FluidForceAmount(const FluidForce& force, float time);

/// @note 気体の発生源が time に注いでいるか: enabled && time >= startTime && (duration <= 0 || time < startTime + duration)。
/// @note (液体の撃ち出しは数で決まるのでソルバーが数える。)
[[nodiscard]] FBZZ_FLUID_API bool FluidSourceEmitting(const FluidSource& source, float time);
/// @note 力が time に効いているか (FluidSourceEmitting と同じ規則)。
[[nodiscard]] FBZZ_FLUID_API bool FluidForceActive(const FluidForce& force, float time);

/// @note 発生源の形の中での注入の重み [0,1] (形の外は 0。ノイズ揺らぎは含まない)。
/// @note minSize: これより小さい寸法はこの値まで広げる (格子ならセル幅)。d = p − center (2D は d.z = 0)、
/// @note a = normalize(direction) (長さ 0 なら (0,1,0))。
/// @note Sphere r=max(size.x,minSize): q²=|d|²/r²、w=(1−q²)²。Box h=max(size,minSize)各軸: q=max(|d|/h)、w=saturate((1−q)×4)
/// @note Cone L,R=size.y,size.x: t=dot(d,a)、[0,L]外は0、ρ=|d−at|、rt=max(Rt/L,minSize/2)、w=(1−ρ²/rt²)²×saturate((1−t/L)×4)
/// @note Ring R,r=size.x,size.y: h=dot(d,a)、ρ=|d−ah|−R、w=(1−(ρ²+h²)/r²)²
/// @note Capsule/Cylinder R,H=size.x,size.y: hc=clamp(dot(d,a),±H) (Cylinder は |h|>H で 0)、ρ=|d−ahc|、w=(1−ρ²/R²)²
/// @note いずれも境界で q² (Box は q) >= 1 なら 0。
[[nodiscard]] FBZZ_FLUID_API float FluidSourceWeight(const FluidSource& source, const math::Vector3& center, const math::Vector3& p,
                                      float minSize, bool volumetric);

/// @note 液体の粒子を湧かせる位置: 形の中でほぼ一様な点。u0..u2 は [0,1) の乱数。2D では z = center.z の
/// @note 断面から選ぶ。Texture は板 (箱) の中の一様な点 (マスクは見ない — 下の関数を使う)。
/// @note Capsule / Cylinder の 3D は体積一様: Cylinder は h=H(2u0−1)、r=R√u1、φ=2πu2 の円柱座標。
/// @note Capsule は円柱と両端半球を体積比で選び分け、半球側は球内の一様点を軸端 (±aH) へ平行移動する。
/// @note 2D は断面の軸方向 α に一様、直交方向 β を α での半幅に一様に選ぶ (端がわずかに濃くなる —
/// @note SampleRing2D と同じ流儀)。
[[nodiscard]] FBZZ_FLUID_API math::Vector3 SampleFluidSourcePoint(const FluidSource& source, const math::Vector3& center,
                                                   float u0, float u1, float u2, bool volumetric);

/// @note Texture の板の軸。n = normalize(direction) (長さ 0 なら (0,0,1) = 手前向き)。
/// @note |n.y| < 0.99 なら right = normalize(cross((0,1,0), n))、そうでなければ right = (1,0,0)。up = cross(n, right)。
/// @note (n = (0,0,1) で right = (1,0,0)、up = (0,1,0) — 画像がそのまま正面に見える)
FBZZ_FLUID_API void FluidTextureSourceBasis(const FluidSource& source, math::Vector3& outRight, math::Vector3& outUp,
                             math::Vector3& outNormal);

/// @note Texture の注入の重み。d = p − center (2D は d.z = 0)、h = max(size, minSize) (各軸)。
/// @note a = dot(d, right)/h.x、b = dot(d, up)/h.y、c = dot(d, n)/h.z。|a|・|b|・|c| のどれかが 1 以上なら 0。
/// @note w = SampleFluidSourceMask(mask, (a + 1)/2, (1 − b)/2) × saturate((1 − |c|) × 4)。
/// @note FluidSourceWeight に Texture を渡した場合はマスク 1 (= 板の形) として扱う。
[[nodiscard]] FBZZ_FLUID_API float FluidTextureSourceWeight(const FluidSource& source, const math::Vector3& center,
                                             const math::Vector3& p, float minSize, bool volumetric,
                                             const FluidSourceMask& mask);

/// @note 液体: Texture の板の中の点を u0..u2 で選び、accept (一様乱数 [0,1)) がその点のマスク値以上なら false
/// @note (呼び手が引き直す。何度か外れたら諦めてよい)。受け入れたら outPoint に入れて true。2D は板の z = center.z 断面。
[[nodiscard]] FBZZ_FLUID_API bool SampleFluidTextureSourcePoint(const FluidSource& source, const math::Vector3& center,
                                                 const FluidSourceMask& mask, float u0, float u1, float u2,
                                                 float accept, bool volumetric, math::Vector3& outPoint);

/// @note 液体の発生源が詰まりすぎるのを避けるために内部で広げる倍率 (1 = そのまま広げない)。
/// @note 同時に発生源の中に居る粒が静止密度の 2 倍までで収まる大きさへ、形を中心から相似に拡大する。
/// @note 公開する理由: 広げたことが UI に出ないと、Inspector の半径 0.06 と実際に湧く大きさが食い違って見える。
[[nodiscard]] FBZZ_FLUID_API float FluidLiquidEmitScale(const FluidSource& source, const FluidLiquidSettings& liquid,
                                         bool volumetric);

/// @note 障害物の time での姿・居るか (発生源と同じ規則)。
[[nodiscard]] FBZZ_FLUID_API FluidOperatorPose PoseFluidCollider(const FluidCollider& collider, float time);
[[nodiscard]] FBZZ_FLUID_API bool FluidColliderActive(const FluidCollider& collider, float time);

/// @note 障害物への符号付き距離 (外が正・中が負)。d = p − center (2D は d.z = 0)。気体の格子はセル中心の
/// @note 距離が 0 未満のセルを固体とする。
/// @note Sphere: |d| − max(size.x, minSize)。Box h=max(size,minSize)各軸: q=|d|−h、|max(q,0)|+min(max(q),0)
/// @note Plane : dot(d,n) (n=normalize(direction)、長さ0なら(0,1,0))。法線側が外
/// @note Capsule R,H=size.x,size.y、a=normalize(direction): hc=clamp(dot(d,a),±H)、|d−a hc|−R
/// @note Cylinder: 同じ R・H・a で h=dot(d,a)、ρ=|d−ah|、dr=ρ−R、dh=|h|−H、|max((dr,dh),0)|+min(max(dr,dh),0)
[[nodiscard]] FBZZ_FLUID_API float FluidColliderDistance(const FluidCollider& collider, const math::Vector3& center,
                                          const math::Vector3& p, float minSize, bool volumetric);

/// @note 表面の外向き単位法線 (距離の勾配)。2D では z を 0 にして正規化し直す (長さ 0 なら (0,1,0))。
/// @note Sphere: d/|d| (|d|<1e-6 なら (0,1,0))。Box: 外なら max(q,0)×sign(d) を正規化、中なら q 最大軸の sign(d)
/// @note Plane : n
/// @note Capsule: (d−a hc)/|d−a hc|。芯の上 (長さ<1e-6) は e1=normalize(cross(a, |a.x|<0.9?(1,0,0):(0,1,0)))
/// @note (決まった手順で 1 つ選ぶ。up を返すと軸が上向きのとき芯に沿って押してしまう)
/// @note Cylinder: u=(d−ah)/|d−ah| (芯の上なら e1)。外なら u max(dr,0)+a sign(h) max(dh,0) を正規化、
/// @note 中なら dr>=dh で u、そうでなければ a sign(h) (Box の同じ規則)
[[nodiscard]] FBZZ_FLUID_API math::Vector3 FluidColliderNormal(const FluidCollider& collider, const math::Vector3& center,
                                                const math::Vector3& p, float minSize, bool volumetric);

/// @brief 力が dt 秒で与える速度の変化 Δv。forceIndex は有効な部品だけを数えた添字 (GPU も同じ順で数える)。
/// @note 呼び出し順: 浮力・全体風・乱流の後、減衰の前に足す (Drag は途中まで足した速度を見る)。
/// @note 強さ→influence の順で掛ける: GPU は定数へ strength×倍率を入れシェーダーが influence を掛けるため、
/// @note 同じ順でないと丸めの分だけ CPU と GPU の絵がずれる。
/// @note d=p−center (2D はd.z=0)、influence=radius>0?(1−dist/radius)^falloffPower:1 (dist>=radius なら0)、
/// @note s=strength×strengthScale×influence、a=normalize(direction) (長さ0なら(1,0,0))。2D は Δv.z=0。
/// @note Wind:a×s×dt  Attract/Repulse:∓(d/dist)×s×dt (dist<1e-5で0)  Vortex:(cross(k,d)正規化)×s×dt
/// @note Noise:CurlNoise(p×freq+noiseOffset+idx依存オフセット)×s×dt  Drag:−velocity×(1−exp(−max(s,0)×dt))
[[nodiscard]] FBZZ_FLUID_API math::Vector3 FluidForceDelta(const FluidForce& force, const math::Vector3& center,
                                            const math::Vector3& p, const math::Vector3& velocity, float time,
                                            float dt, const math::Vector3& noiseOffset, int forceIndex,
                                            bool volumetric, float strengthScale);

} /// @note namespace fbzz::fluid
