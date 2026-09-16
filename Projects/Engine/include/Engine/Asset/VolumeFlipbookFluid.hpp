/// @file    VolumeFlipbookFluid.hpp
/// @brief   .fluid の気体を 3D で解き、Volume Flipbook Baker の入力 (媒質・速度ボリューム) にする
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// Volume Flipbook Baker の下流 (レイマーチ・MV・Atlas) は «u0 に媒質、u1 に速度» しか見ない
/// (VolumeFill.cs.hlsl の注記)。ここは FluidGasSolver の格子を同じ形へ詰め、VolumeUpload.cs.hlsl が
/// そのまま u0 / u1 へ写す。
/// 座標はそのまま一致する: ソルバーは «最長軸 = [-1,1]・y 上向き» なので、立方体で解けば
/// bake 空間 [-1,1]^3 と同じ。速度の単位も «領域単位/秒» = bake 単位/秒。
#pragma once

#include <Engine/Asset/FluidRecipe.hpp>
#include <Engine/Asset/FluidSolver.hpp>
#include <Engine/Asset/FluidStepping.hpp>
#include <Math/Vector4.hpp>

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::asset {

/// 1 コマぶんのボリューム。並びは x が最も速い (x + n·(y + n·z))。VolumeUpload.cs.hlsl と同じ。
struct PackedFluidVolume {
    int resolution = 0;
    /// 何コマ目か。-1 = まだ何も入っていない。
    int frame = -1;
    /// R 密度 / G 温度 / B colorKey / A 液体の割合 (VolumeFill と同じ意味)
    std::vector<math::Vector4> medium;
    /// xyz 速度 [bake 単位/秒]
    std::vector<math::Vector4> velocity;
};

struct FluidVolumeScale {
    /// 媒質 R = 密度 × density
    float density = 1.0f;
    /// 媒質 G = 温度 × temperature (レイマーチは G を [0,1] の «炎の温度» として読む)
    float temperature = 1.0f;
};

/// 解いた格子を詰める。ソルバーは立方体 (nx = ny = nz) で解いてあること (違えば out は空)。
void PackFluidVolume(const FluidGasSolver& solver, const FluidVolumeScale& scale, PackedFluidVolume& out);

/// 3D の液体粒子をボリュームへ塗る。粒子ごとに半径 (粒子半径 × radiusScale) の山を足し、
/// VolumeRaymarch はその和が threshold を跨ぐところを液面として描く。A (液体の割合) は 1。
/// 速度と B (粒子の colorKey) は山の重みで平均する。lifetime > 0 なら寿命の終わりへ向けて山を細らせる。
void PackLiquidVolume(const FluidLiquidSolver& solver, int resolution, float radiusScale, float lifetime,
                      PackedFluidVolume& out);

/// 発生源の芯の温度が 1 になる倍率 (発生源が無ければ 1)。
[[nodiscard]] float FluidRecipeTemperatureScale(const FluidRecipe& recipe);

/// 3D の流体 (気体の格子 / 液体の粒子) を 1 コマずつ別スレッドで解いて渡す。
/// 0 コマ目 = warmup の後に 1 コマぶん進めた状態 (FluidBaker の 2D ベイクと同じ数え方)。
/// 先のコマは続きから解き、手前へ戻るコマは最初から解き直す。
class FluidVolumeStream {
public:
    FluidVolumeStream() = default;
    ~FluidVolumeStream();
    FluidVolumeStream(const FluidVolumeStream&) = delete;
    FluidVolumeStream& operator=(const FluidVolumeStream&) = delete;

    /// 気体も液体も開ける。解き始めはしない (Request で始まる)。
    [[nodiscard]] bool Open(const FluidRecipe& recipe, int resolution, float frameDt, float densityScale,
                            std::string& outError);
    /// 解いている最中なら終わるまで待ってから閉じる。
    void Close();
    /// 走っているワーカーに «途中でやめてよい» と伝えるだけ (待たない)。
    /// WHY: 96³ の 1 コマは数秒かかる。ソルバーを切り替えた直後の 1 コマはどのみち捨てるので、
    ///      解き終わるのを待つ理由が無い。待つと切り替えや終了のたびにエディターごと止まる。
    /// 畳まれたコマは Poll から返らず、続きの場も捨てて次の Request で頭から解き直す。
    void Cancel() noexcept;
    [[nodiscard]] bool IsOpen() const { return m_solver != nullptr || m_liquid != nullptr; }
    [[nodiscard]] bool Busy() const { return m_job.valid(); }
    /// frame コマ目を解き始める。解いている最中なら何もせず false。
    bool Request(int frame);
    /// 解き終わっていれば out へ移して true。待たない。
    [[nodiscard]] bool Poll(PackedFluidVolume& out);

private:
    FluidRecipe m_recipe;
    std::unique_ptr<FluidGasSolver> m_solver;
    std::unique_ptr<FluidLiquidSolver> m_liquid;
    float m_liquidRadiusScale = 2.5f;
    float m_liquidLifetime = 0.0f;
    FluidVolumeScale m_scale;
    int m_resolution = 0;
    float m_frameDt = 1.0f / 24.0f;
    int m_substeps = 1;
    /// 焼き始めの前に進めるコマ数 (FluidWarmupFrames)。warmup も普通のコマとして解く。
    int m_warmupFrames = 0;
    /// warmup の後に進めたコマ数 (frame コマ目は frame + 1 回進めた状態)。-1 = まだ Reset していない。
    /// 解いている間は別スレッドだけが触る (Request は Busy の間は何もしない)。
    int m_advancedFrames = -1;
    /// 走っているワーカーと共有する中止の旗。Request ごとに新しく作る。
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::future<PackedFluidVolume> m_job;
};

} // namespace fbzz::asset
