/// @file    ElectricMinusParticleComponent.hpp
/// @brief   −極の電極。自分の粒子を芯から押し出し、＋極の粒子を吸い寄せる。
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// 組み立ても運動も ElectrodeRig が持つ。ここが決めるのは極の符号と調整値だけで、
/// ElectricPlusParticleComponent とは符号以外まったく同じ振る舞いをする。
///
/// WHY 色をここで持たないか:
///   赤 = ＋ / 青 = − は ElectrodePole.hpp が全アセット共通の制約として持っている。
///   タイトル画面だけ別の青を選べるようにすると、盤面の色の意味とタイトルの色が食い違う。
///
/// WHY 放電 (Arc) の欄がここに無いか:
///   放電は 1 本の極ではなく «対» の持ち物で、両極が張ると同じ 2 点に 2 束が重なる。
///   担当を ＋極 に決めてあるので、調整欄も ElectricPlusParticleComponent 側だけにある。
///
/// WHY カーソル追従 (Cursor) の欄がここに無いか:
///   カーソルは画面に 1 つしかない。両極が同じ点を目指すと重なって 1 点に潰れ、
///   引き合いも放電も出なくなる。掴まれる側を ＋極 に決めて、−極は追う側に残す。
#pragma once

#include <Scripts/Title/ElectrodeRig.hpp>

namespace sandbox {

class ElectricMinusParticleComponent : public Script {
    FBZZ_SCRIPT(ElectricMinusParticleComponent)

public:
    FBZZ_GROUP("パーティクル")
    FBZZ_FIELD_ENUM(ParticleSimulationMode, simulationMode, ParticleSimulationMode::Cpu,
                    "Simulation", "CPU", "GPU")
    FBZZ_TOOLTIP("粒子をどちらで回すか。GPU は数千粒を超えたときだけ効く。"
                 "素材は経路に合わせて自動で切り替わる (ElectricChargeMinus[GPU].mat)。"
                 "縮退したときは理由が 1 回だけ警告に出る")
    FBZZ_FIELD_RANGE_INT(int, maxParticles, 1200, "粒子の上限", 16, 60000)
    FBZZ_TOOLTIP("この電極が同時に出せる粒子数。加算合成では重なり枚数がそのまま白飛びに効く")
    FBZZ_FIELD_RANGE(float, emitRate, 200.0f, "発生レート", 0.0f, 5000.0f)
    FBZZ_TOOLTIP("毎秒の発生数。上げるほど重なりが増え、色が白へ抜けていく")
    FBZZ_FIELD_RANGE(float, lifetime, 2.4f, "寿命", 0.05f, 10.0f)
    FBZZ_FIELD_RANGE(float, particleSize, 0.45f, "粒子の大きさ", 0.001f, 3.0f)
    FBZZ_TOOLTIP("1 粒の大きさ。大きくしたら Emit Rate を下げて重なりを保つこと")
    FBZZ_FIELD_RANGE(float, spawnRadius, 0.55f, "発生半径", 0.01f, 6.0f)
    FBZZ_TOOLTIP("湧き出し口の半径。狭いほど芯が密集して白く飽和する")
    FBZZ_FIELD_RANGE(float, crackle, 3.0f, "はぜる音", 0.0f, 30.0f)
    FBZZ_TOOLTIP("カールノイズの強さ。放電のちらつきを作る。0 でまっすぐ流れる")
    FBZZ_FIELD_RANGE(float, radialBurst, 3.0f, "放射バースト", -30.0f, 30.0f)
    FBZZ_TOOLTIP("芯から外向きの加速度。負で吸い込みになる")
    FBZZ_FIELD_RANGE(float, spin, -2.0f, "回転", -30.0f, 30.0f)
    FBZZ_TOOLTIP("極を軸にした粒子の周回。＋と逆向きに回すと対で編み込まれて見える")
    FBZZ_FIELD_RANGE(float, hotCore, 1.2f, "発生時のブースト", 0.05f, 3.0f)
    FBZZ_TOOLTIP("生まれた瞬間の明るさ倍率。色は極性色のまま明るさだけ立ち上がる")

    FBZZ_GROUP("コア端子")
    FBZZ_FIELD_RANGE(float, coreSize, 0.62f, "記号の大きさ", 0.0f, 4.0f)
    FBZZ_TOOLTIP("輪の中に刻む − の差し渡し [m]。0 で端子ごと出さない")
    FBZZ_FIELD_RANGE(float, coreThickness, 0.13f, "記号の太さ", 0.01f, 1.0f)
    FBZZ_TOOLTIP("符号の腕の太さ [m]")
    FBZZ_FIELD_RANGE(float, coreRingScale, 1.85f, "輪のスケール", 1.0f, 5.0f)
    FBZZ_TOOLTIP("輪の直径 ÷ Symbol Size。1 に近づけると符号が輪へ触れて紋章が潰れる")
    FBZZ_FIELD_RANGE(float, coreRingBeads, 5.0f, "輪の粒", 0.0f, 40.0f)
    FBZZ_TOOLTIP("輪を回る粒の数。0 で粒を出さない")
    FBZZ_FIELD_RANGE(float, coreRingTravel, -1.2f, "輪の粒の速さ", -30.0f, 30.0f)
    FBZZ_TOOLTIP("粒が輪を回る速さ。1 周は Ring Beads ÷ この値 [秒]。＋と逆に回すと対で噛み合う")
    FBZZ_FIELD_RANGE(float, coreIntensity, 2.8f, "端子の強さ", 0.0f, 20.0f)
    FBZZ_TOOLTIP("端子の明るさ。粒子の靄に負けると符号が読めない")
    FBZZ_FIELD_RANGE(float, corePulseRate, 1.6f, "脈動の速さ", 0.0f, 20.0f)
    FBZZ_TOOLTIP("端子の明滅の速さ [Hz]")
    FBZZ_FIELD_RANGE(float, corePulseDepth, 0.18f, "脈動の深さ", 0.0f, 1.0f)
    FBZZ_TOOLTIP("明滅の深さ。0 で止まる")

    FBZZ_GROUP("力線")
    FBZZ_FIELD_RANGE_INT(int, fieldLineCount, 10, "線の本数", 0, 32)
    FBZZ_TOOLTIP("芯から出る電気力線の本数。0 で出さない。"
                 "1 本ごとに LineRenderer が 1 つ増えるので、描画コストはここが一番効く")
    FBZZ_FIELD_RANGE_INT(int, fieldLineSegments, 44, "線の分割数", 4, 64)
    FBZZ_TOOLTIP("1 本を何回の積分で描くか。曲がりの滑らかさと引き換えに CPU コストが下がる。"
                 "Line Length を伸ばしたら増やさないと刻みが粗くなる")
    FBZZ_FIELD_RANGE(float, fieldLineLength, 9.0f, "線の長さ", 0.0f, 30.0f)
    FBZZ_TOOLTIP("線の道のり [m]。直線距離ではないので、曲がるぶん到達点は近い。"
                 "＋極まで届いた線は赤で終わって対を結び、届かない線は先細りして消える")
    FBZZ_FIELD_RANGE(float, fieldLineStart, 0.75f, "線の開始半径", 0.05f, 6.0f)
    FBZZ_TOOLTIP("湧き出し口の半径 [m]。輪 (Symbol Size × Ring Scale ÷ 2) より外に置くこと")
    FBZZ_FIELD_RANGE(float, fieldLineWidth, 0.055f, "線の幅", 0.005f, 1.0f)
    FBZZ_FIELD_RANGE(float, fieldLineIntensity, 1.1f, "線の強さ", 0.0f, 12.0f)
    FBZZ_TOOLTIP("力線の明るさ。放電より弱くしないと «細い放電» にしか見えない")
    FBZZ_FIELD_RANGE(float, fieldLineBeads, 4.0f, "線の粒", 0.0f, 40.0f)
    FBZZ_TOOLTIP("線 1 本を流れる粒の数。＋極へ届いた線にだけ乗る。0 で粒を出さない")
    FBZZ_FIELD_RANGE(float, fieldLineTravel, 1.1f, "線の進み", 0.0f, 30.0f)
    FBZZ_TOOLTIP("粒の速さ。−極では外から芯へ向かって流れ込む")
    FBZZ_FIELD_RANGE(float, fieldLineSpin, -7.0f, "線の回転", -180.0f, 180.0f)
    FBZZ_TOOLTIP("線束全体を回す角速度 [deg/s]。＋と逆向きに回すと対で噛み合って見える")

    FBZZ_GROUP("Field")
    FBZZ_FIELD_RANGE(float, pullStrength, 26.0f, "引き (異極)", 0.0f, 200.0f)
    FBZZ_TOOLTIP("＋極の粒子をこの電極へ吸い込む加速度")
    FBZZ_FIELD_RANGE(float, pushStrength, 10.0f, "押し (同極)", 0.0f, 200.0f)
    FBZZ_TOOLTIP("−極の粒子をこの電極から押し出す加速度。芯からの湧き出しを作る")
    FBZZ_FIELD_RANGE(float, fieldRadius, 10.0f, "力場の半径", 0.0f, 100.0f)
    FBZZ_TOOLTIP("場の到達距離。0 以下でシーン全体へ減衰なしに効く")
    FBZZ_FIELD_RANGE(float, swirlStrength, -4.0f, "渦", -50.0f, 50.0f)
    FBZZ_TOOLTIP("極を軸にした渦。＋と逆向きに回すと 2 本の流れが編み込まれて見える")

    FBZZ_GROUP("動き")
    FBZZ_FIELD_RANGE(float, coupling, 5.0f, "結合", 0.0f, 80.0f)
    FBZZ_TOOLTIP("電極どうしが引き合う / 反発する強さ。0 で電極は動かない")
    FBZZ_FIELD_RANGE(float, homeSpring, 2.0f, "戻りのばね", 0.0f, 40.0f)
    FBZZ_TOOLTIP("元の配置へ戻すばね。0 にすると電極が画面外へ流れ去る")
    FBZZ_FIELD_RANGE(float, minSeparation, 1.6f, "最小の間隔", 0.05f, 20.0f)
    FBZZ_TOOLTIP("逆極どうしがこれ以上近づかない距離。重なって 1 点に潰れるのを防ぐ")
    FBZZ_FIELD_RANGE(float, orbitSpeed, 8.0f, "旋回の速さ", -180.0f, 180.0f)
    FBZZ_TOOLTIP("原点まわりに定位置を回す角速度 [deg/s]。釣り合った後も画面を止めない")

    void OnStart()   override { m_rig.Attach(*this, Pole::Minus, Tuning()); }
    void OnUpdate()  override { m_rig.Tick(*this, Tuning(), time.UnscaledDeltaTime()); }
    void OnDestroy() override { m_rig.Detach(*this); }

private:
    [[nodiscard]] ElectrodeTuning Tuning() const
    {
        ElectrodeTuning tuning;
        tuning.simulationMode = simulationMode;
        tuning.maxParticles  = maxParticles;
        tuning.emitRate      = emitRate;
        tuning.lifetime      = lifetime;
        tuning.sizeStart     = particleSize;
        tuning.spawnRadius   = spawnRadius;
        // 記号が読めなくなる手前で止める。0 まで絞ると開く / 畳む瞬間が «点滅» に見える。
        tuning.sizeEnd       = particleSize * 0.30f;
        tuning.crackle       = crackle;
        tuning.radialBurst   = radialBurst;
        tuning.spin          = spin;
        tuning.hotCore       = hotCore;

        tuning.core.size       = coreSize;
        tuning.core.thickness  = coreThickness;
        tuning.core.ringScale  = coreRingScale;
        tuning.core.ringBeads  = coreRingBeads;
        tuning.core.ringTravel = coreRingTravel;
        tuning.core.intensity  = coreIntensity;
        tuning.core.pulseRate  = corePulseRate;
        tuning.core.pulseDepth = corePulseDepth;

        tuning.field.lineCount   = fieldLineCount;
        tuning.field.segments    = fieldLineSegments;
        tuning.field.length      = fieldLineLength;
        tuning.field.startRadius = fieldLineStart;
        tuning.field.width       = fieldLineWidth;
        tuning.field.intensity   = fieldLineIntensity;
        tuning.field.beads       = fieldLineBeads;
        tuning.field.travel      = fieldLineTravel;
        tuning.field.spin        = fieldLineSpin;
        // arc の設定はここに無い。放電は対の持ち物で ＋極の rig だけが張るため
        // (ElectrodeRig::UpdateArcs)、−側に同じ欄を出しても何も起きない。
        // 記号と力線は逆に «1 個の電荷» の持ち物なので、両極が同じ欄を持つ。
        tuning.pullStrength  = pullStrength;
        tuning.pushStrength  = pushStrength;
        tuning.fieldRadius   = fieldRadius;
        tuning.swirlStrength = swirlStrength;
        tuning.coupling      = coupling;
        tuning.homeSpring    = homeSpring;
        tuning.minSeparation = minSeparation;
        tuning.orbitSpeed    = orbitSpeed;
        return tuning;
    }

    ElectrodeRig m_rig;
};

FBZZ_REFLECT(ElectricMinusParticleComponent)

} // namespace sandbox
