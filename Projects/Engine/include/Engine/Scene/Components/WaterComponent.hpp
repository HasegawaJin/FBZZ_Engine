/// @file    WaterComponent.hpp
/// @brief   水面コンポーネント（ジオメトリ・水の種類 (.mat) の参照・浮力・着水）。
/// @author  Hasegawa Jin
/// @date    2026-06-01
///
/// 水の «種類» ── 色・さざ波・Gerstner 波・風への反応・水流 ── は materialPath が指す
/// .mat が丸ごと持つ。コンポーネントに残すのは «この 1 枚» ごとに違う量だけ。
/// WHY 波まで .mat に置くか: 色だけ .mat にあり波がコンポーネントにあると、Ocean.mat を
///     差しても湖の波のまま、という食い違いが起きる。水の種類は 1 ファイルで決まるべき。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Physics/BodyHandle.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace fbzz::scene {

// GerstnerWave — 水面を構成する 1 本の波のパラメータ。
// WHY: 水面全体をシミュレーションせず、少数の解析波を合成することで低コストに大きなうねりを表現する。
struct GerstnerWave {
    math::Vector2 direction  = { 1.0f, 0.0f }; // 進行方向。正規化は評価側で行う。
    float         amplitude  = 0.30f;          // 波高 A [m]
    float         wavelength = 10.0f;          // 波長 lambda [m]
    float         steepness  = 0.50f;          // 急峻度 Q [0,1]。1 を超えると波面が交差する。
};

// WaterComponent — シーン上の水面 1 面分。
struct WaterComponent {
    // ── ジオメトリ ──────────────────────────────────────────────────────────
    uint32_t resolutionX = 64;
    uint32_t resolutionZ = 64;
    float    extentX     = 100.0f;
    float    extentZ     = 100.0f;
    // チャンク分割数。水面を chunkCount×chunkCount のサブメッシュに分割し、
    // チャンク単位でフラスタムカリングする。
    uint32_t chunkCount = 4;

    // ── 波 (個体ごとの補正。波そのものは .mat が持つ) ──────────────────────
    bool  enableGerstnerWaves = true;
    /// .mat の振幅に掛ける倍率。同じ Ocean.mat を «凪の入り江» と «外洋» に使い分けるため。
    float waveAmplitudeScale  = 1.0f;

    // ── 物理 ────────────────────────────────────────────────────────────────
    bool  buoyancyEnabled = true;
    /// 完全に沈んだときの上向き加速度 [m/s^2]。重力 (9.8) を超えると浮く。
    float buoyancy        = 15.0f;
    /// 水中での速度減衰 [1/s]。水流があるときは «水に対する» 速度に掛かる。
    float waterDrag       = 2.0f;
    /// 浮力が届く水面からの深さ [m]。
    /// WHY 上限を置くか: 水面は厚みを持たない板なので、置いたままだと «水面の真下にある洞窟»
    ///     の中まで浮力が届いてしまう。
    float buoyancyDepth   = 10.0f;
    /// 剛体が水面を通過したときに波紋としぶきを出す。
    bool  splashEnabled   = true;

    // ── 水の種類 ────────────────────────────────────────────────────────────
    std::string materialPath;

    // ── 状態フラグ ──────────────────────────────────────────────────────────
    bool enabled   = true;
    bool meshDirty = true;
    bool foamDirty = true;
    bool texDirty  = true;

    // ── ランタイム (保存しない) ─────────────────────────────────────────────
    /// WaterSystem が毎フレーム «.mat の波 × 倍率 × 環境風» から組み立てた実効波。
    /// 描画・浮力・水中判定・スクリプトはすべてこれを読む。
    std::array<GerstnerWave, 4> waves = {};
    /// 水流の速度 [m/s] (ワールド XZ)。.mat の flowDirection × currentSpeed。
    math::Vector2 current = math::Vector2::ZERO;
    /// WaterSystem が毎フレーム書く «頂点グリッド 1 セルのワールド実寸» [m]。
    /// WHY: 刻めない波長の Gerstner 波を寝かせる判断に、描画 (GPU) と浮力 (CPU) が
    ///      同じ値を使う必要がある。片方だけ寝かせると、平らな水面の上で物が揺れる。
    math::Vector2 cellSize = math::Vector2::ZERO;
    /// 波の «群» の深さ [0,1]。.mat の waveGrouping をそのまま持つ。
    float waveGrouping = 0.0f;
    /// 方向広がり [0,1]。.mat の waveSpread をそのまま持つ。0 で «1 波 1 方向»。
    float waveSpread = 0.0f;
    physics::VolumeHandle volumeHandle;

    const char* GetTypeName() const { return "Water"; }

    /// 頂点グリッドで «刻めない» 波を寝かせる係数 [0,1]。Water.hlsl の WaveMeshFade と同じ式。
    ///
    /// WHY: Gerstner 波は頂点でしか評価されないので、1 波長あたり数セルしか取れない波は
    ///      山と谷がセル境界で入れ替わり、«もっと長い別の波» に化ける。海サイズの水面では
    ///      これが全面で起きる。刻めない波は消し、細かさは手続きさざ波に任せる。
    /// @param cell 頂点グリッド 1 セルのワールド実寸 [m]。0 のときはフェードしない。
    static float WaveMeshFade(float wavelength, math::Vector2 cell)
    {
        const float c = (std::max)(cell.x, cell.y);
        if (c <= 0.0f) return 1.0f;
        // smoothstep(2, 3.5, 1 波長あたりのセル数)。下限 2.0 は Nyquist。
        const float t = math::Clamp01((wavelength / c - 2.0f) / 1.5f);
        return t * t * (3.0f - 2.0f * t);
    }

    /// 群の向きを波からどれだけ傾けるか (rad 0.55 の cos / sin)。Water.hlsl と同じ値。
    /// WHY 傾けるか: 群が波と同じ向きに進むだけだと、波頭は «高さの揃った無限に長い直線» の
    ///      ままになる。斜めにずらすと包絡が波頭に沿っても変化し、うねりが塊へ割れる。
    static constexpr float WAVE_GROUP_COS = 0.852525f;
    static constexpr float WAVE_GROUP_SIN = 0.522687f;

    /// 波の «群» の包絡 [1-depth, 1+depth]。振幅にそのまま掛ける。
    ///
    /// WHY 要るか: 正弦を 4 本足しただけの水面はどの波頭も同じ高さ・同じ形になる。実海面は
    ///      近い周波数どうしの «うなり» で波が群れて進み、大きい波の塊と凪の区間が交互に来る。
    /// @param index 波の番号 [0,3]。群の波長と傾ける向きをここから決める (CB を増やさないため)。
    /// @note 群の角周波数を r*omega/2 にするのは、深水波の群速度が位相速度の 1/2 だから。
    ///       これで «群はゆっくり進み、個々の波頭がその中を追い越していく» 見え方になる。
    /// @warning Water.hlsl の WaveGroupEnvelope と同じ式であること。片方だけ変えると、
    ///          見えている波と浮力が別の水面になる。
    static float WaveGroupEnvelope(int index, math::Vector2 direction, float k, float omega,
                                   float worldX, float worldZ, float time, float depth)
    {
        if (depth <= 0.0001f) return 1.0f;
        const float ratio = 0.09f + 0.035f * static_cast<float>(index);
        const float sign  = (index & 1) ? -1.0f : 1.0f;
        const math::Vector2 groupDir = {
            direction.x * WAVE_GROUP_COS - direction.y * sign * WAVE_GROUP_SIN,
            direction.x * sign * WAVE_GROUP_SIN + direction.y * WAVE_GROUP_COS
        };
        const float groupK = ratio * k;
        const float phase = groupK * (groupDir.x * worldX + groupDir.y * worldZ)
                          - ratio * omega * 0.5f * time;
        return 1.0f + depth * std::sin(phase);
    }

    /// 方向広がりの成分 1 つぶんの «向きと振幅倍率»。
    struct WaveSpreadPart {
        math::Vector2 direction;
        float         amplitudeScale = 1.0f;
    };

    /// 方向広がりの成分を割り出す。
    ///
    /// @param component 0 = 主成分, 1 = 伴走成分。
    /// @note 実海面の波は 1 方向へ揃わず狭い方向スペクトルを持つ。同じ波数で向きだけ違う波を
    ///       重ねると、波頭が有限の長さに切れる (short-crested sea)。
    /// @note 振幅は main^2 + comp^2 = 1 で分ける。分けないと spread を上げるだけで海が高くなる。
    /// @warning Water.hlsl の ApplyWaveSpread / ResolveWaveSpread と同じ式であること。
    static WaveSpreadPart WaveSpreadComponent(int index, int component, math::Vector2 direction,
                                              float spread)
    {
        const float clamped = math::Clamp01(spread);
        const float energy = math::Clamp01(0.40f * clamped);
        WaveSpreadPart part;
        part.direction = direction;
        if (component == 0) {
            part.amplitudeScale = std::sqrt(1.0f - energy);
            return part;
        }
        part.amplitudeScale = std::sqrt(energy);
        const float angle = clamped * 0.9f * ((index & 1) ? -1.0f : 1.0f);
        const float ca = std::cos(angle);
        const float sa = std::sin(angle);
        part.direction = { direction.x * ca - direction.y * sa,
                           direction.x * sa + direction.y * ca };
        return part;
    }

    /// 主成分と伴走成分の振幅倍率の «和»。AABB のマージンと波高の基準に使う。
    static float WaveSpreadAmplitudeSum(float spread)
    {
        const float energy = math::Clamp01(0.40f * math::Clamp01(spread));
        return std::sqrt(1.0f - energy) + std::sqrt(energy);
    }

    /// 逆写像の反復回数。
    /// @note 減衰を波の急峻度から決めるので、収束する設定 (Σ Q·k·A ≤ 1) では素の不動点反復に
    ///       なり、4 回で残差が mm 台へ入る。
    static constexpr int SURFACE_SOLVE_ITERATIONS = 4;

    /// Gerstner 波の水平変位 [m] (ワールド XZ)。
    /// @param undisplaced 変位«前»の水平位置。波の位相はここで取る。
    math::Vector2 HorizontalDisplacementAt(math::Vector2 undisplaced, float time) const
    {
        math::Vector2 offset = math::Vector2::ZERO;
        if (!enableGerstnerWaves) return offset;

        for (int i = 0; i < 4; ++i) {
            const GerstnerWave& wave = waves[static_cast<size_t>(i)];
            if (wave.amplitude < 0.0001f || wave.wavelength <= 0.0001f) continue;
            const float fade = WaveMeshFade(wave.wavelength, cellSize);
            if (fade <= 0.0f) continue;
            const float k = math::TWO_PI / wave.wavelength;
            const float omega = std::sqrt(9.8f * k);

            for (int component = 0; component < 2; ++component) {
                const WaveSpreadPart part =
                    WaveSpreadComponent(i, component, wave.direction.Normalized(), waveSpread);
                if (part.amplitudeScale <= 0.001f) continue;
                const math::Vector2 dir = part.direction;
                const float envelope = WaveGroupEnvelope(i, dir, k, omega,
                                                         undisplaced.x, undisplaced.y, time, waveGrouping);
                const float phase = k * (dir.x * undisplaced.x + dir.y * undisplaced.y) - omega * time;
                const float push = math::Clamp01(wave.steepness) * wave.amplitude * fade
                                 * part.amplitudeScale * envelope * std::cos(phase);
                offset.x += dir.x * push;
                offset.y += dir.y * push;
            }
        }
        return offset;
    }

    /// ワールド XZ に «水面が来ている» とき、その点の変位前の水平位置を解く。
    ///
    /// WHY 要るか: Gerstner 波は «変位前の位置» を位相の入力に取る。ワールド XZ をそのまま位相へ
    ///      入れると、水平変位のぶんだけ別の場所の高さを読むことになり、急な斜面では Q·A の合計
    ///      (Ocean.mat では 1〜2 m) ずれる。浮力・水中判定・スクリプトが揃ってずれる。
    /// @warning 折り畳んだ波では逆写像が一意でない (同じ XZ に複数の水面がある)。反復はその枝の
    ///          1 つへ落ちる。どれになるかは初期値依存で、連続性は保証しない。
    math::Vector2 SolveUndisplacedXZ(float worldX, float worldZ, float time) const
    {
        const math::Vector2 target = { worldX, worldZ };
        if (!enableGerstnerWaves) return target;

        // 不動点反復の縮小率の上界 Σ Q·k·A。三角関数を 1 度も回さずに出せる。
        // 群の包絡は最大 (1 + waveGrouping) 倍、方向広がりは主 + 伴走の和まで振幅を持ち上げる。
        const float peak = (1.0f + math::Clamp01(waveGrouping)) * WaveSpreadAmplitudeSum(waveSpread);
        float contraction = 0.0f;
        for (const GerstnerWave& wave : waves) {
            if (wave.amplitude < 0.0001f || wave.wavelength <= 0.0001f) continue;
            const float fade = WaveMeshFade(wave.wavelength, cellSize);
            contraction += math::Clamp01(wave.steepness) * wave.amplitude * fade * peak
                         * (math::TWO_PI / wave.wavelength);
        }
        // 水平変位を持たない水面 (湖・プール・急峻度 0) は解く必要がない。
        if (contraction <= 0.0001f) return target;

        // Σ Q·k·A ≤ 1 なら素の反復がそのまま縮小写像。超える («波が折り畳む») 設定では
        // その比で減衰させ、有限回で暴れないところへ抑える。
        const float relax = 1.0f / (std::max)(contraction, 1.0f);

        math::Vector2 guess = target;
        for (int iteration = 0; iteration < SURFACE_SOLVE_ITERATIONS; ++iteration) {
            const math::Vector2 offset = HorizontalDisplacementAt(guess, time);
            guess.x += (target.x - (guess.x + offset.x)) * relax;
            guess.y += (target.y - (guess.y + offset.y)) * relax;
        }
        return guess;
    }

    /// 水面の基準面からの高さ [m] を CPU で評価する。
    /// @param worldX, worldZ ワールド座標 (変位«後»、つまり画面に出ている位置)。
    /// WHY ワールド座標か: シェーダーは波の位相をワールド XZ で取る。水面の原点からの相対座標で
    ///     評価すると、水面を原点以外へ置いた瞬間に浮力・水中判定と描画の波がずれる。
    /// @see SolveUndisplacedXZ
    float GetSurfaceHeightAt(float worldX, float worldZ, float time) const
    {
        if (!enableGerstnerWaves) return 0.0f;

        const math::Vector2 origin = SolveUndisplacedXZ(worldX, worldZ, time);
        float height = 0.0f;
        for (int i = 0; i < 4; ++i) {
            const GerstnerWave& wave = waves[static_cast<size_t>(i)];
            if (wave.amplitude < 0.0001f || wave.wavelength <= 0.0001f) continue;
            // 描画が寝かせた波は水面の形にも出ない。同じ係数を掛けないと、平らに見える
            // 遠くの海面の上で浮いている物だけが波に乗って上下する。
            const float fade = WaveMeshFade(wave.wavelength, cellSize);
            if (fade <= 0.0f) continue;
            const float k = math::TWO_PI / wave.wavelength;
            const float omega = std::sqrt(9.8f * k);

            for (int component = 0; component < 2; ++component) {
                const WaveSpreadPart part =
                    WaveSpreadComponent(i, component, wave.direction.Normalized(), waveSpread);
                if (part.amplitudeScale <= 0.001f) continue;
                const math::Vector2 dir = part.direction;
                const float envelope = WaveGroupEnvelope(i, dir, k, omega,
                                                         origin.x, origin.y, time, waveGrouping);
                const float phase = k * (dir.x * origin.x + dir.y * origin.y) - omega * time;
                height += wave.amplitude * fade * part.amplitudeScale * envelope * std::sin(phase);
            }
        }
        return height;
    }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        const uint32_t oldResolutionX = resolutionX;
        const uint32_t oldResolutionZ = resolutionZ;
        const uint32_t oldChunkCount  = chunkCount;
        const float oldExtentX = extentX;
        const float oldExtentZ = extentZ;
        const std::string oldMaterialPath = materialPath;

        // IReflector は uint32_t 非対応のため int 経由で編集・保存する。
        int resX = static_cast<int>((std::clamp)(resolutionX, 1u, 512u));
        int resZ = static_cast<int>((std::clamp)(resolutionZ, 1u, 512u));
        int chunks = static_cast<int>((std::clamp)(chunkCount, 1u, 64u));
        r.Field("resolutionX", resX);
        r.Field("resolutionZ", resZ);
        r.Field("chunkCount", chunks);
        r.Field("extentX", extentX);
        r.Field("extentZ", extentZ);
        r.Field("materialPath", materialPath);
        r.Field("enableGerstnerWaves", enableGerstnerWaves);
        r.Field("waveAmplitudeScale", waveAmplitudeScale);
        r.Field("buoyancyEnabled", buoyancyEnabled);
        r.Field("buoyancy", buoyancy);
        r.Field("waterDrag", waterDrag);
        r.Field("buoyancyDepth", buoyancyDepth);
        r.Field("splashEnabled", splashEnabled);

        resolutionX = static_cast<uint32_t>((std::clamp)(resX, 1, 512));
        resolutionZ = static_cast<uint32_t>((std::clamp)(resZ, 1, 512));
        chunkCount  = static_cast<uint32_t>((std::clamp)(chunks, 1, 64));
        extentX = (std::clamp)(extentX, 0.1f, 10000.0f);
        extentZ = (std::clamp)(extentZ, 0.1f, 10000.0f);
        waveAmplitudeScale = (std::max)(waveAmplitudeScale, 0.0f);
        buoyancyDepth      = (std::max)(buoyancyDepth, 0.1f);
        if (resolutionX != oldResolutionX || resolutionZ != oldResolutionZ ||
            chunkCount  != oldChunkCount  || extentX      != oldExtentX      ||
            extentZ     != oldExtentZ) {
            meshDirty = true;
            foamDirty = true;
        }
        if (materialPath != oldMaterialPath) {
            texDirty = true;
            foamDirty = true;
        }
    }
};

} // namespace fbzz::scene
