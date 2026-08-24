/// @file    ElectricPlusParticleComponent.hpp
/// @brief   ＋極の電極。自分の粒子を芯から押し出し、−極の粒子を吸い寄せる。
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// 組み立ても運動も ElectrodeRig が持つ。ここが決めるのは極の符号と調整値だけで、
/// ElectricMinusParticleComponent とは符号以外まったく同じ振る舞いをする。
///
/// WHY 色をここで持たないか:
///   赤 = ＋ / 青 = − は PolarityTypes.hpp が全アセット共通の制約として持っている。
///   タイトル画面だけ別の赤を選べるようにすると、盤面の色の意味とタイトルの色が食い違う。
#pragma once

#include <Scripts/Title/ElectrodeRig.hpp>

namespace sandbox {

class ElectricPlusParticleComponent : public Script {
    FBZZ_SCRIPT(ElectricPlusParticleComponent)

public:
    FBZZ_GROUP("Particles")
    FBZZ_FIELD_ENUM(ParticleSimulationMode, simulationMode, ParticleSimulationMode::Cpu,
                    "Simulation", "CPU", "GPU")
    FBZZ_TOOLTIP("粒子をどちらで回すか。GPU は数千粒を超えたときだけ効く。"
                 "素材は経路に合わせて自動で切り替わる (ElectricChargePlus[GPU].mat)。"
                 "縮退したときは理由が 1 回だけ警告に出る")
    FBZZ_FIELD_RANGE_INT(int, maxParticles, 1200, "Max Particles", 16, 60000)
    FBZZ_TOOLTIP("この電極が同時に出せる粒子数。加算合成では重なり枚数がそのまま白飛びに効く")
    FBZZ_FIELD_RANGE(float, emitRate, 200.0f, "Emit Rate", 0.0f, 5000.0f)
    FBZZ_TOOLTIP("毎秒の発生数。上げるほど重なりが増え、色が白へ抜けていく")
    FBZZ_FIELD_RANGE(float, lifetime, 2.4f, "Lifetime", 0.05f, 10.0f)
    FBZZ_FIELD_RANGE(float, particleSize, 0.45f, "Particle Size", 0.001f, 3.0f)
    FBZZ_TOOLTIP("1 粒の大きさ。大きくしたら Emit Rate を下げて重なりを保つこと")
    FBZZ_FIELD_RANGE(float, spawnRadius, 0.55f, "Spawn Radius", 0.01f, 6.0f)
    FBZZ_TOOLTIP("湧き出し口の半径。狭いほど芯が密集して白く飽和する")
    FBZZ_FIELD_RANGE(float, crackle, 3.0f, "Crackle", 0.0f, 30.0f)
    FBZZ_TOOLTIP("カールノイズの強さ。放電のちらつきを作る。0 でまっすぐ流れる")
    FBZZ_FIELD_RANGE(float, radialBurst, 3.0f, "Radial Burst", -30.0f, 30.0f)
    FBZZ_TOOLTIP("芯から外向きの加速度。負で吸い込みになる")
    FBZZ_FIELD_RANGE(float, spin, 2.0f, "Spin", -30.0f, 30.0f)
    FBZZ_TOOLTIP("極を軸にした粒子の周回。湧き出し口で巻いてから飛ぶ")
    FBZZ_FIELD_RANGE(float, hotCore, 1.2f, "Birth Boost", 0.05f, 3.0f)
    FBZZ_TOOLTIP("生まれた瞬間の明るさ倍率。色は極性色のまま明るさだけ立ち上がる")

    FBZZ_GROUP("Core Terminal")
    FBZZ_FIELD_RANGE(float, coreSize, 0.62f, "Symbol Size", 0.0f, 4.0f)
    FBZZ_TOOLTIP("輪の中に刻む ＋ の差し渡し [m]。0 で端子ごと出さない")
    FBZZ_FIELD_RANGE(float, coreThickness, 0.13f, "Symbol Thickness", 0.01f, 1.0f)
    FBZZ_TOOLTIP("符号の腕の太さ [m]")
    FBZZ_FIELD_RANGE(float, coreRingScale, 1.85f, "Ring Scale", 1.0f, 5.0f)
    FBZZ_TOOLTIP("輪の直径 ÷ Symbol Size。1 に近づけると符号が輪へ触れて紋章が潰れる")
    FBZZ_FIELD_RANGE(float, coreRingBeads, 5.0f, "Ring Beads", 0.0f, 40.0f)
    FBZZ_TOOLTIP("輪を回る粒の数。0 で粒を出さない")
    FBZZ_FIELD_RANGE(float, coreRingTravel, 1.2f, "Ring Bead Speed", -30.0f, 30.0f)
    FBZZ_TOOLTIP("粒が輪を回る速さ。1 周は Ring Beads ÷ この値 [秒]。負で逆回り")
    FBZZ_FIELD_RANGE(float, coreIntensity, 2.8f, "Terminal Intensity", 0.0f, 20.0f)
    FBZZ_TOOLTIP("端子の明るさ。粒子の靄に負けると符号が読めない")
    FBZZ_FIELD_RANGE(float, corePulseRate, 1.6f, "Pulse Rate", 0.0f, 20.0f)
    FBZZ_TOOLTIP("端子の明滅の速さ [Hz]")
    FBZZ_FIELD_RANGE(float, corePulseDepth, 0.18f, "Pulse Depth", 0.0f, 1.0f)
    FBZZ_TOOLTIP("明滅の深さ。0 で止まる")

    FBZZ_GROUP("Field Lines")
    FBZZ_FIELD_RANGE_INT(int, fieldLineCount, 10, "Line Count", 0, 32)
    FBZZ_TOOLTIP("芯から出る電気力線の本数。0 で出さない。"
                 "1 本ごとに LineRenderer が 1 つ増えるので、描画コストはここが一番効く")
    FBZZ_FIELD_RANGE_INT(int, fieldLineSegments, 44, "Line Segments", 4, 64)
    FBZZ_TOOLTIP("1 本を何回の積分で描くか。曲がりの滑らかさと引き換えに CPU コストが下がる。"
                 "Line Length を伸ばしたら増やさないと刻みが粗くなる")
    FBZZ_FIELD_RANGE(float, fieldLineLength, 9.0f, "Line Length", 0.0f, 30.0f)
    FBZZ_TOOLTIP("線の道のり [m]。直線距離ではないので、曲がるぶん到達点は近い。"
                 "−極まで届いた線は青で終わって対を結び、届かない線は先細りして消える")
    FBZZ_FIELD_RANGE(float, fieldLineStart, 0.75f, "Line Start Radius", 0.05f, 6.0f)
    FBZZ_TOOLTIP("湧き出し口の半径 [m]。輪 (Symbol Size × Ring Scale ÷ 2) より外に置くこと")
    FBZZ_FIELD_RANGE(float, fieldLineWidth, 0.055f, "Line Width", 0.005f, 1.0f)
    FBZZ_FIELD_RANGE(float, fieldLineIntensity, 1.1f, "Line Intensity", 0.0f, 12.0f)
    FBZZ_TOOLTIP("力線の明るさ。放電より弱くしないと «細い放電» にしか見えない")
    FBZZ_FIELD_RANGE(float, fieldLineBeads, 4.0f, "Line Beads", 0.0f, 40.0f)
    FBZZ_TOOLTIP("線 1 本を流れる粒の数。−極へ届いた線にだけ乗る。0 で粒を出さない")
    FBZZ_FIELD_RANGE(float, fieldLineTravel, 1.1f, "Line Travel", 0.0f, 30.0f)
    FBZZ_TOOLTIP("粒の速さ。＋極では芯から外向き (−極へ) 流れる")
    FBZZ_FIELD_RANGE(float, fieldLineSpin, 7.0f, "Line Spin", -180.0f, 180.0f)
    FBZZ_TOOLTIP("線束全体を回す角速度 [deg/s]。0 で止まる")

    FBZZ_GROUP("Arc")
    FBZZ_FIELD_RANGE_INT(int, arcStrands, 3, "Strands", 0, 8)
    FBZZ_TOOLTIP("放電の筋の本数。0 で放電を出さない")
    FBZZ_FIELD_RANGE(float, arcAmplitude, 0.55f, "Amplitude", 0.0f, 5.0f)
    FBZZ_TOOLTIP("折れの振れ幅。両端は 0 で中央が最大")
    FBZZ_FIELD_RANGE(float, arcWidth, 0.10f, "Width", 0.005f, 1.0f)
    FBZZ_FIELD_RANGE(float, arcStrikeRate, 22.0f, "Strike Rate", 1.0f, 60.0f)
    FBZZ_TOOLTIP("形を組み替える頻度 [Hz]。上げすぎると白色雑音に見える")
    FBZZ_FIELD_RANGE(float, arcRange, 7.0f, "Range", 0.0f, 40.0f)
    FBZZ_TOOLTIP("この距離を超えると放電が消える。極が近づいたときだけ走らせる")
    FBZZ_FIELD_RANGE(float, arcIntensity, 1.6f, "Intensity", 0.0f, 12.0f)
    FBZZ_FIELD_RANGE(float, arcBreakup, 0.55f, "Breakup", 0.0f, 1.0f)
    FBZZ_TOOLTIP("芯の途切れ。1 に近いほど筋が断続する")

    FBZZ_GROUP("Field")
    FBZZ_FIELD_RANGE(float, pullStrength, 26.0f, "Pull (opposite)", 0.0f, 200.0f)
    FBZZ_TOOLTIP("−極の粒子をこの電極へ吸い込む加速度")
    FBZZ_FIELD_RANGE(float, pushStrength, 10.0f, "Push (same)", 0.0f, 200.0f)
    FBZZ_TOOLTIP("＋極の粒子をこの電極から押し出す加速度。芯からの湧き出しを作る")
    FBZZ_FIELD_RANGE(float, fieldRadius, 10.0f, "Field Radius", 0.0f, 100.0f)
    FBZZ_TOOLTIP("場の到達距離。0 以下でシーン全体へ減衰なしに効く")
    FBZZ_FIELD_RANGE(float, swirlStrength, 4.0f, "Swirl", -50.0f, 50.0f)
    FBZZ_TOOLTIP("極を軸にした渦。吸い込まれる直線を弧に曲げる")

    FBZZ_GROUP("Motion")
    FBZZ_FIELD_RANGE(float, coupling, 5.0f, "Coupling", 0.0f, 80.0f)
    FBZZ_TOOLTIP("電極どうしが引き合う / 反発する強さ。0 で電極は動かない")
    FBZZ_FIELD_RANGE(float, homeSpring, 2.0f, "Home Spring", 0.0f, 40.0f)
    FBZZ_TOOLTIP("元の配置へ戻すばね。0 にすると電極が画面外へ流れ去る")
    FBZZ_FIELD_RANGE(float, minSeparation, 1.6f, "Min Separation", 0.05f, 20.0f)
    FBZZ_TOOLTIP("逆極どうしがこれ以上近づかない距離。重なって 1 点に潰れるのを防ぐ")
    FBZZ_FIELD_RANGE(float, orbitSpeed, 8.0f, "Orbit Speed", -180.0f, 180.0f)
    FBZZ_TOOLTIP("原点まわりに定位置を回す角速度 [deg/s]。釣り合った後も画面を止めない")

    FBZZ_GROUP("Cursor")
    FBZZ_FIELD(bool, followCursor, false, "Follow Cursor")
    FBZZ_TOOLTIP("ゲーム内カーソルの指す画面点へこの電極を寄せる。"
                 "奥行きはシーンで置いた位置のまま保つので、画面のどこへ動かしても大きさは変わらない。"
                 "入れると Home Spring は効かなくなる (行き先がカーソルになるため)")
    FBZZ_FIELD_RANGE(float, followResponse, 12.0f, "Follow Response", 0.1f, 40.0f)
    FBZZ_TOOLTIP("カーソルへの食いつき [rad/s]。臨界減衰なので上げても行き過ぎない。"
                 "小さいほど重く遅れて付いてくる")

    void OnStart()   override { m_rig.Attach(*this, Polarity::Plus, Tuning()); }
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

        tuning.arc.strandCount = arcStrands;
        tuning.arc.amplitude   = arcAmplitude;
        tuning.arc.width       = arcWidth;
        tuning.arc.strikeRate  = arcStrikeRate;
        tuning.arc.strikeRange = arcRange;
        tuning.arc.intensity   = arcIntensity;
        tuning.arc.breakup     = arcBreakup;

        tuning.pullStrength  = pullStrength;
        tuning.pushStrength  = pushStrength;
        tuning.fieldRadius   = fieldRadius;
        tuning.swirlStrength = swirlStrength;
        tuning.coupling      = coupling;
        tuning.homeSpring    = homeSpring;
        tuning.minSeparation = minSeparation;
        tuning.orbitSpeed    = orbitSpeed;

        tuning.followCursor   = followCursor;
        tuning.followResponse = followResponse;
        return tuning;
    }

    ElectrodeRig m_rig;
};

FBZZ_REFLECT(ElectricPlusParticleComponent)

} // namespace sandbox
