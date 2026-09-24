/// @file    FluidPreviewCache.cpp
/// @brief   Fluid Editor の 2D ライブプレビュー — 裏のスレッドで解き、コマとソルバーの途中経過を取っておく
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// @note 部品が変わっても途中経過から続けられる: FluidInvalidationTime より前は新旧レシピの解きが 1 ビットも違わないため、その手前の途中経過は新レシピでも通用する。ReplaceOperators で部品だけを差し替え (場・粒子・時計・乱数は保持) し、変わり目の手前から続きを解く。格子の意味が変わるレシピは頭から解き直す。
/// @note 続きから解いた結果と頭から解き直した結果がずれるのは «変わり目の見積もりが遅すぎた» ときだけで、液体は汚れが変わり目より後へ粒子として残り得る。PartInvalidation の「値が 1 つでも違う部品はキーを見ずに startTime まで戻す」という縛りでこの差を抑えている。緩めるときはここを見直す。
#include <Editor/Util/FluidPreviewCache.hpp>

#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/FluidBaker.hpp>
#include <Engine/Scene/Components/ParticleColorSpace.hpp>
#include <Fluid/FluidSolver.hpp>
#include <Fluid/FluidSourceMask.hpp>
#include <Fluid/FluidStepping.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace fbzz::editor {
namespace {

using fluid::FluidKind;
using fluid::FluidShading;

/// @note 部品が変わっていない。
constexpr float kNever = (std::numeric_limits<float>::max)();
/// @note 頭から効く。
constexpr float kAlways = std::numeric_limits<float>::lowest();

constexpr int kDraftGrid      = 48;
constexpr int kNormalGrid     = 96;
constexpr int kFinalMaxGrid   = 256;
/// @note 画像の一辺の上限 [画素]。実寸はビューポートに出している一辺から決める — 格子より大きく描くのは
/// @note Catmull-Rom の補間が受け持つので、画像だけ上げても解きは重くならない。
constexpr int kDraftMaxImage  = 256;
constexpr int kNormalMaxImage = 384;
constexpr int kFinalMaxImage  = 512;
constexpr int kMinImage       = 32;
/// @note 画像の一辺の下限。出す一辺がまだ分からない (一度も描いていない) ときもこれで描く。
constexpr int kBaseImage      = 128;
/// @note 刻む: 画像の一辺は «描き直しが要るか» の鍵で、1 画素ごとに追うと窓を伸縮している間ずっと全コマ描き直し (途中経過の無いコマは解き直し) になる。
constexpr int kImageStep      = 64;
constexpr float kMaxViewSide  = 8192.0f;
constexpr float kMaxWarmup    = 30.0f;

/// @note 予算を持つ: コマ数は最大 32×32 = 1024。Final の 512px を float で全部持つと 4GB、256² 格子の途中経過 (作業領域込みで 1 つ約 8MB) を全コマ持つと 8GB になる。
constexpr std::size_t kImageBudgetBytes    = std::size_t{ 128 } << 20;
constexpr std::size_t kSnapshotBudgetBytes = std::size_t{ 128 } << 20;

/// @note 積算した時計の丸め誤差で «変わり目ちょうどのコマ» を残してしまわないための幅。
constexpr float kTimeEpsilon = 1.0e-4f;

/// @name 比較
/// @note 自前で比べる: math::Vector3::operator== は NearlyEqual (許容誤差つき) で、ドラッグで少しずつ動かした値を «同じ» と見なすと解き直しが要るのに古いコマが残る。

bool Same(float a, float b) { return a == b; }
bool Same(const math::Vector3& a, const math::Vector3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool Same(const math::Vector4& a, const math::Vector4& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

/// @note 量のエンベロープはキーの並びごと比べる。startTime より前には効かないので、部品の «値が変わった»
/// @note 扱い (PartInvalidation の fieldsDiffer) にしておけば startTime から解き直して足りる。
bool Same(const fluid::FluidAmount& a, const fluid::FluidAmount& b)
{
    if (a.keys.size() != b.keys.size()) return false;
    for (std::size_t i = 0; i < a.keys.size(); ++i)
        if (!Same(a.keys[i].time, b.keys[i].time) || !Same(a.keys[i].scale, b.keys[i].scale)) return false;
    return true;
}

bool Same(const fluid::FluidColorRamp& a, const fluid::FluidColorRamp& b)
{
    for (std::size_t i = 0; i < a.stops.size(); ++i)
        if (!Same(a.stops[i].color, b.stops[i].color) || !Same(a.stops[i].position, b.stops[i].position)) return false;
    return true;
}

bool GasDiffers(const fluid::FluidGasSettings& a, const fluid::FluidGasSettings& b)
{
    return a.resolution != b.resolution || !Same(a.buoyancy, b.buoyancy) || !Same(a.weight, b.weight)
        || !Same(a.vorticity, b.vorticity) || !Same(a.turbulence, b.turbulence)
        || !Same(a.turbulenceScale, b.turbulenceScale) || !Same(a.densityDissipation, b.densityDissipation)
        || !Same(a.temperatureDissipation, b.temperatureDissipation) || !Same(a.velocityDamping, b.velocityDamping)
        || !Same(a.ignitionTemperature, b.ignitionTemperature) || !Same(a.burnRate, b.burnRate)
        || !Same(a.burnHeat, b.burnHeat) || !Same(a.burnSmoke, b.burnSmoke) || !Same(a.burnExpansion, b.burnExpansion)
        || !Same(a.wind, b.wind) || a.floor != b.floor || a.pressureIterations != b.pressureIterations
        || a.sharpAdvection != b.sharpAdvection || !Same(a.detailPeriod, b.detailPeriod);
}

bool LiquidDiffers(const fluid::FluidLiquidSettings& a, const fluid::FluidLiquidSettings& b)
{
    return a.maxParticles != b.maxParticles || !Same(a.particleRadius, b.particleRadius)
        || !Same(a.gravity, b.gravity) || !Same(a.viscosity, b.viscosity) || !Same(a.cohesion, b.cohesion)
        || a.solverIterations != b.solverIterations || a.floor != b.floor || !Same(a.floorHeight, b.floorHeight)
        || !Same(a.floorFriction, b.floorFriction) || !Same(a.particleLifetime, b.particleLifetime);
}

/// @note 2D プレビューのコマの刻み・格子・解像度に効くものだけ。supersampling / loop / motionVectors / vectorField* は
/// @note 焼きの段でしか使わない (プレビューは超解像もループのクロスフェードもしない)。
bool OutputDiffers(const fluid::FluidOutputSettings& a, const fluid::FluidOutputSettings& b)
{
    return a.frameSize != b.frameSize || a.columns != b.columns || a.rows != b.rows
        || !Same(a.duration, b.duration) || !Same(a.warmup, b.warmup) || a.substeps != b.substeps;
}

/// @note 名前は解き方に関係しない。種類ごとの «使わない値» (気体の count など) まで比べるのは、使っていないことを
/// @note ソルバーの中身に頼って決めないため (無駄な解き直しは起きても、要る解き直しを落とさない)。
bool SourceDiffers(const fluid::FluidSource& a, const fluid::FluidSource& b)
{
    return a.shape != b.shape || !Same(a.center, b.center) || !Same(a.size, b.size)
        || !Same(a.direction, b.direction) || a.texture != b.texture || !Same(a.density, b.density)
        || !Same(a.temperature, b.temperature) || !Same(a.fuel, b.fuel) || !Same(a.noise, b.noise)
        || !Same(a.velocity, b.velocity) || !Same(a.startTime, b.startTime) || !Same(a.duration, b.duration)
        || !Same(a.colorKey, b.colorKey) || !Same(a.spread, b.spread) || a.count != b.count
        || !Same(a.amount, b.amount);
}

bool ForceDiffers(const fluid::FluidForce& a, const fluid::FluidForce& b)
{
    return a.type != b.type || !Same(a.center, b.center) || !Same(a.direction, b.direction)
        || !Same(a.strength, b.strength) || !Same(a.radius, b.radius) || !Same(a.falloffPower, b.falloffPower)
        || !Same(a.noiseFrequency, b.noiseFrequency) || !Same(a.noiseSpeed, b.noiseSpeed)
        || !Same(a.startTime, b.startTime) || !Same(a.duration, b.duration) || !Same(a.amount, b.amount);
}

bool ColliderDiffers(const fluid::FluidCollider& a, const fluid::FluidCollider& b)
{
    return a.shape != b.shape || !Same(a.center, b.center) || !Same(a.size, b.size)
        || !Same(a.direction, b.direction) || !Same(a.friction, b.friction)
        || !Same(a.startTime, b.startTime) || !Same(a.duration, b.duration);
}

/// @note 動きが変わって姿が変わり始める時刻。キーの間は直線で結ぶので、i 番目のキーを変えると
/// @note «1 つ前のキー» から先の道筋が変わる。先頭を変えると先頭より前 (端のキーで止まっている間) も変わる。
float MotionInfluenceStart(const fluid::FluidMotion& a, const fluid::FluidMotion& b)
{
    if (a.inheritVelocity != b.inheritVelocity) return kAlways;
    const std::size_t common = (std::min)(a.keys.size(), b.keys.size());
    std::size_t first = common;
    for (std::size_t i = 0; i < common; ++i) {
        if (!Same(a.keys[i].time, b.keys[i].time) || !Same(a.keys[i].offset, b.keys[i].offset)) {
            first = i;
            break;
        }
    }
    if (first == common && a.keys.size() == b.keys.size()) return kNever;
    if (first == 0) return kAlways;
    /// @note 昇順が崩れたキー (編集途中) でも取りこぼさないよう、以降のキーの最小を取る。
    float start = kNever;
    for (std::size_t i = first - 1; i < a.keys.size(); ++i) start = (std::min)(start, a.keys[i].time);
    for (std::size_t i = first - 1; i < b.keys.size(); ++i) start = (std::min)(start, b.keys[i].time);
    return start;
}

/// @note 部品 1 つが解きに効き始める時刻 [ソルバーの時計]。
/// @note enabled の切り替えは頭から: ソルバーは有効な部品だけを詰めて持ち、ノイズの切り出し位置が «有効な部品の中での添字» で決まるため、1 つ消すと後ろの部品のノイズが最初から変わる。
template <class Part>
float PartInvalidation(const Part& a, const Part& b, bool fieldsDiffer)
{
    if (!a.enabled && !b.enabled) return kNever;
    if (a.enabled != b.enabled) return kAlways;
    const float start = (std::min)(a.startTime, b.startTime);
    if (fieldsDiffer) return start;
    const float motion = MotionInfluenceStart(a.motion, b.motion);
    if (motion == kNever) return kNever;
    /// @note 居ない間 (startTime より前) の姿は解きに効かない。
    return (std::max)(start, motion);
}

/// @note 焼きと同じく、Glow の Ramp を温度で引くかは «温度か燃料を注ぐ発生源が 1 つでもあるか» で決まる。
/// @note 時刻に関係なく全コマの色が変わるので、解きは同じでも描き直しが要る。
bool GlowRampFollowsTemperature(const fluid::FluidRecipe& recipe)
{
    return std::any_of(recipe.sources.begin(), recipe.sources.end(), [](const fluid::FluidSource& source) {
        return source.enabled && (source.temperature > 0.0f || source.fuel > 0.0f);
    });
}

/// @note 2D の描画 (RenderFluidGasFrame / RenderFluidLiquidFrame) が読む Look の値。
/// @note liquidSoftness / Extinction / Gloss / Fresnel は 3D (Volume Flipbook Baker) だけが読む。
bool RenderDiffers(const fluid::FluidRenderSettings& a, const fluid::FluidRenderSettings& b)
{
    return a.shading != b.shading || !Same(a.smokeColor, b.smokeColor) || !Same(a.shadowColor, b.shadowColor)
        || !Same(a.opacity, b.opacity) || !Same(a.selfShadow, b.selfShadow)
        || !Same(a.lightDirection, b.lightDirection) || !Same(a.detailStrength, b.detailStrength)
        || !Same(a.detailScale, b.detailScale) || !Same(a.fireKelvin, b.fireKelvin)
        || !Same(a.fireIntensity, b.fireIntensity) || a.useEmissionRamp != b.useEmissionRamp
        || !Same(a.emissionRamp, b.emissionRamp) || a.useAlbedoRamp != b.useAlbedoRamp
        || !Same(a.albedoRamp, b.albedoRamp) || !Same(a.liquidColor, b.liquidColor)
        || !Same(a.liquidRadiusScale, b.liquidRadiusScale) || !Same(a.liquidThreshold, b.liquidThreshold)
        || !Same(a.specular, b.specular);
}

bool LookDiffers(const fluid::FluidRecipe& before, const fluid::FluidRecipe& after)
{
    return RenderDiffers(before.render, after.render)
        || GlowRampFollowsTemperature(before) != GlowRampFollowsTemperature(after);
}

/// @note どちらかが解き直しを求めていれば、早い方を取る。
float CombineInvalidation(float a, float b)
{
    if (a < 0.0f) return b;
    if (b < 0.0f) return a;
    return (std::min)(a, b);
}

/// @name プレビューのレシピ

FluidShading EffectiveShading(const fluid::FluidRecipe& recipe)
{
    if (recipe.kind == FluidKind::Liquid) return FluidShading::Liquid;
    return recipe.render.shading == FluidShading::Liquid ? FluidShading::Smoke : recipe.render.shading;
}

/// @note 焼き (BakeFluid) と同じ丸め。違えるとコマの時刻が焼いたアトラスとずれる。
fluid::FluidRecipe NormalizedForPreview(const fluid::FluidRecipe& source)
{
    fluid::FluidRecipe recipe = source;
    fluid::NormalizeFluidOutput(recipe.output);
    recipe.render.shading = EffectiveShading(recipe);
    return recipe;
}

int FrameCountOf(const fluid::FluidRecipe& normalized)
{
    return normalized.output.columns * normalized.output.rows;
}

float FrameDtOf(const fluid::FluidRecipe& normalized)
{
    return normalized.output.duration / static_cast<float>(FrameCountOf(normalized));
}

struct PreviewScale {
    int grid  = kDraftGrid;
    int image = kBaseImage;
};

/// @note 画面に出す一辺 [画素] に見合う画像の一辺。
int ImageSideFor(float viewSide, int cap)
{
    if (!(viewSide > 0.0f)) return kBaseImage;
    const float clamped = (std::min)(viewSide, kMaxViewSide);
    const int steps = static_cast<int>(std::ceil(clamped / static_cast<float>(kImageStep)));
    return std::clamp(steps * kImageStep, kBaseImage, cap);
}

PreviewScale ScaleFor(const fluid::FluidRecipe& normalized, FluidPreviewQuality quality, int frameCount,
                      float viewSide)
{
    /// @note 利用者が Auto より粗い格子を指定していれば、それより細かくはしない (焼きより細かいプレビューは嘘になる)。
    const int resolved = fluid::ResolveGasResolution(normalized);
    PreviewScale scale;
    switch (quality) {
    case FluidPreviewQuality::Draft:
        scale = { (std::min)(resolved, kDraftGrid), ImageSideFor(viewSide, kDraftMaxImage) };
        break;
    case FluidPreviewQuality::Normal:
        scale = { (std::min)(resolved, kNormalGrid), ImageSideFor(viewSide, kNormalMaxImage) };
        break;
    case FluidPreviewQuality::Final:
        /// @note Final は «焼きと同じコマの大きさ» を下回らない。画面がそれより大きいときだけ上限まで足す
        /// @note       (プレビューの画素数は焼きのコマの大きさとは別物なので、超えて構わない)。
        scale = { (std::min)(resolved, kFinalMaxGrid),
                  (std::max)((std::min)(normalized.output.frameSize, kFinalMaxImage),
                             ImageSideFor(viewSide, kFinalMaxImage)) };
        break;
    }
    /// @note 1 画素 = float4 (motion は捨てて持つ)。コマ数が多いときだけ解像度を落として予算に収める。
    const double perFrame = static_cast<double>(kImageBudgetBytes)
                          / (static_cast<double>((std::max)(frameCount, 1)) * 4.0 * sizeof(float));
    const int budgetSide = static_cast<int>(std::floor(std::sqrt(perFrame)));
    scale.image = (std::max)((std::min)(scale.image, budgetSide), kMinImage);
    return scale;
}

/// @note 途中経過 1 つの大きさの見積もり。ソルバーのコピーは作業領域まで丸ごと写す。
std::size_t EstimateSnapshotBytes(const fluid::FluidRecipe& normalized, int grid)
{
    if (normalized.kind == FluidKind::Gas) {
        /// @note 場 (密度・温度・燃料・色・速度 3・圧力・発散・膨張) + 作業領域 (前の速度 3・scratch 3・curl 4・
        /// @note       固体の速度 3・細部の座標 4) でおよそ 30 本 + 固体フラグ。
        const std::size_t cells = static_cast<std::size_t>(grid) * static_cast<std::size_t>(grid);
        std::size_t bytes = cells * (30 * sizeof(float) + 1);
        const std::size_t maskBytes = static_cast<std::size_t>(fluid::kFluidSourceMaskSize)
                                    * static_cast<std::size_t>(fluid::kFluidSourceMaskSize) * sizeof(float);
        for (const fluid::FluidSource& source : normalized.sources)
            if (source.enabled && source.shape == fluid::FluidSourceShape::Texture) bytes += maskBytes;
        return bytes;
    }
    std::size_t particles = 0;
    for (const fluid::FluidSource& source : normalized.sources)
        if (source.enabled) particles += static_cast<std::size_t>((std::max)(source.count, 0));
    particles = (std::min)(particles, static_cast<std::size_t>((std::max)(normalized.liquid.maxParticles, 0)));
    /// @note 粒子本体 32B + 予測位置・λ・密度・補正・格子の添字 + 近傍リスト (20 近傍ほど)。
    return particles * 192 + (std::size_t{ 64 } << 10);
}

/// @name 裏のスレッド

struct SolverSnapshot {
    /// @note 作ったときの場 (格子・粒子半径) の世代。今と同じときだけ «続きを解く» 起点に使える。
    std::uint64_t simEpoch = 0;
    /// @note 作ったときの部品の世代。今と違うなら、続ける前に ReplaceOperators で部品を差し替える。
    std::uint64_t partEpoch = 0;
    fluid::FluidGasSolver    gas;
    fluid::FluidLiquidSolver liquid;
};

struct Produced {
    int index = 0;
    bool hasImage = false;
    asset::FluidFrameImage image;
    std::shared_ptr<const SolverSnapshot> snapshot;
};

struct Channel {
    std::atomic<bool> cancel{ false };
    std::atomic<bool> done{ false };
    std::mutex mutex;
    std::vector<Produced> produced;

    [[nodiscard]] bool Cancelled() const { return cancel.load(std::memory_order_relaxed); }

    void Push(Produced&& item)
    {
        std::lock_guard<std::mutex> lock(mutex);
        produced.push_back(std::move(item));
    }

    [[nodiscard]] std::vector<Produced> Take()
    {
        std::vector<Produced> taken;
        std::lock_guard<std::mutex> lock(mutex);
        taken.swap(produced);
        return taken;
    }
};

struct Job {
    fluid::FluidRecipe recipe;
    FluidKind kind = FluidKind::Gas;
    int grid = kDraftGrid;
    int imageSize = kBaseImage;
    float frameDt = 0.0f;
    int substeps = 1;
    /// @note 焼き始めまでに進めるコマ数 (FluidWarmupFrames)。
    int warmupFrames = 0;
    std::uint64_t simEpoch = 0;
    std::uint64_t partEpoch = 0;

    /// @note 1 段目: 途中経過から描き直すだけのコマ (Look の変更)。解かないので速い。
    std::vector<std::pair<int, std::shared_ptr<const SolverSnapshot>>> rerender;

    /// @note 2 段目: resumeFrom (無ければ Reset + warmup でコマ 0) から lastIndex まで解く。負なら解かない。
    std::shared_ptr<const SolverSnapshot> resumeFrom;
    /// @note resumeFrom の部品が古い。続ける前に差し替える (断られたら頭から解き直す)。
    bool replaceOperators = false;
    int resumeIndex = 0;
    int lastIndex = -1;
    std::vector<std::uint8_t> render;
    std::vector<std::uint8_t> keepSnapshot;
};

void RenderWith(const fluid::FluidGasSolver& solver, const Job& job, asset::FluidFrameImage& out)
{
    asset::RenderFluidGasFrame(solver, job.recipe, job.imageSize, job.frameDt, out);
}

void RenderWith(const fluid::FluidLiquidSolver& solver, const Job& job, asset::FluidFrameImage& out)
{
    asset::RenderFluidLiquidFrame(solver, job.recipe, job.imageSize, job.frameDt, out);
}

/// @note プレビューは motion を使わない。持つ量が 2/3 になる。
void DropMotion(asset::FluidFrameImage& image)
{
    std::vector<float>().swap(image.motion);
}

template <class Solver>
void SolveStage(const Job& job, Channel& channel)
{
    constexpr bool kGas = std::is_same_v<Solver, fluid::FluidGasSolver>;
    Solver solver;
    int index = 0;
    bool resumed = false;
    if (job.resumeFrom != nullptr) {
        /// @note 別の入れ物へ復元する: 差し替えを断られたソルバーは部品を途中まで入れ替えた状態かもしれないため、そのまま Reset に回さず丸ごと捨てて空のソルバーから組み直す。
        Solver restored;
        if constexpr (kGas) restored = job.resumeFrom->gas;
        else                restored = job.resumeFrom->liquid;
        if (!job.replaceOperators || restored.ReplaceOperators(job.recipe)) {
            solver = std::move(restored);
            index = job.resumeIndex;
            resumed = true;
        }
    }
    const float stepDt = job.frameDt / static_cast<float>(job.substeps);
    if (!resumed) {
        if constexpr (kGas) solver.Reset(job.recipe, job.grid, job.grid, 1);
        else                solver.Reset(job.recipe);
        /// @note warmup も «普通のコマ» として解く (刻みの正本は FluidStepping)。
        for (int i = 0; i < job.warmupFrames; ++i) {
            for (int step = 0; step < job.substeps; ++step) {
                if (channel.Cancelled()) return;
                solver.Advance(stepDt);
            }
        }
    }

    for (;;) {
        const auto slot = static_cast<std::size_t>(index);
        const bool render = job.render[slot] != 0;
        const bool keep = job.keepSnapshot[slot] != 0;
        if (render || keep) {
            Produced produced;
            produced.index = index;
            if (render) {
                RenderWith(solver, job, produced.image);
                DropMotion(produced.image);
                produced.hasImage = true;
            }
            if (keep) {
                std::shared_ptr<SolverSnapshot> snapshot = std::make_shared<SolverSnapshot>();
                snapshot->simEpoch = job.simEpoch;
                snapshot->partEpoch = job.partEpoch;
                if constexpr (kGas) snapshot->gas = solver;
                else                snapshot->liquid = solver;
                produced.snapshot = std::move(snapshot);
            }
            channel.Push(std::move(produced));
        }
        if (index >= job.lastIndex) return;
        for (int step = 0; step < job.substeps; ++step) {
            if (channel.Cancelled()) return;
            solver.Advance(stepDt);
        }
        ++index;
    }
}

void RunJob(const Job& job, Channel& channel)
{
    for (const auto& [index, snapshot] : job.rerender) {
        if (channel.Cancelled()) break;
        Produced produced;
        produced.index = index;
        if (job.kind == FluidKind::Gas) RenderWith(snapshot->gas, job, produced.image);
        else                            RenderWith(snapshot->liquid, job, produced.image);
        DropMotion(produced.image);
        produced.hasImage = true;
        channel.Push(std::move(produced));
    }
    if (!channel.Cancelled() && job.lastIndex >= 0) {
        if (job.kind == FluidKind::Gas) SolveStage<fluid::FluidGasSolver>(job, channel);
        else                            SolveStage<fluid::FluidLiquidSolver>(job, channel);
    }
    channel.done.store(true, std::memory_order_release);
}

/// @name 表示

/// @note 矩形を並べて描かない: 128px で 16384 個の矩形になり、しかも 1 画素が拡大表示の 1 マスになるので «焼いたものより粗く» 見える。
struct PreviewTexture {
    renderer::ResourceHandle<renderer::TextureTag> handle;
    std::uint64_t resetVersion = 0;
    int   size = 0;
    void* textureId = nullptr;
    bool  unsupported = false;
};

struct Slot {
    asset::FluidFrameImage image;
    /// @note 絵がある (stale でも見せる)。
    bool has = false;
    /// @note 絵は今の Look で描いたものではない (描き直し待ち)。
    bool stale = false;
    FluidShading shading = FluidShading::Smoke;
    /// @note 絵を差し替えるたびに増える。GPU へ上げ直すかの判定に使う。
    std::uint64_t version = 0;
    /// @note このコマ時点のソルバー (予算内で数コマおき)。
    std::shared_ptr<const SolverSnapshot> snapshot;
};

}

ImU32 FluidPreviewBackgroundPixel(int x, int y, bool checkerBackground)
{
    if (!checkerBackground) return IM_COL32(8, 8, 10, 255);
    const bool light = (((x / 8) + (y / 8)) & 1) != 0;
    const int channel = light ? 56 : 31;
    return IM_COL32(channel, channel, channel, 255);
}

void CompositeFluidPreviewPixel(const float rgba[4], int x, int y, int imageSide, float viewSidePixels,
                                fluid::FluidShading shading, bool checkerBackground, float outRgb[3])
{
    const float imageToView = imageSide > 0 && viewSidePixels > 0.0f
        ? viewSidePixels / static_cast<float>(imageSide) : 1.0f;
    const int backgroundX = static_cast<int>((static_cast<float>(x) + 0.5f) * imageToView);
    const int backgroundY = static_cast<int>((static_cast<float>(y) + 0.5f) * imageToView);
    const ImU32 backgroundColor = FluidPreviewBackgroundPixel(backgroundX, backgroundY, checkerBackground);
    const float background[3] = {
        scene::ParticleSrgbToLinear(static_cast<float>((backgroundColor >> IM_COL32_R_SHIFT) & 0xFFu) / 255.0f),
        scene::ParticleSrgbToLinear(static_cast<float>((backgroundColor >> IM_COL32_G_SHIFT) & 0xFFu) / 255.0f),
        scene::ParticleSrgbToLinear(static_cast<float>((backgroundColor >> IM_COL32_B_SHIFT) & 0xFFu) / 255.0f),
    };
    const bool premultiplied = asset::FluidShadingIsPremultiplied(shading);
    const bool additive = shading == fluid::FluidShading::Glow;
    const float alpha = std::clamp(rgba[3], 0.0f, 1.0f);
    for (int channel = 0; channel < 3; ++channel) {
        const float source = scene::ParticleSrgbToLinear(rgba[channel]);
        float value = 0.0f;
        if (premultiplied) value = source + background[channel] * (1.0f - alpha);
        else if (additive) value = background[channel] + source * alpha;
        else               value = source * alpha + background[channel] * (1.0f - alpha);
        outRgb[channel] = scene::ParticleLinearToSrgb(std::clamp(value, 0.0f, 1.0f));
    }
}

float FluidInvalidationTime(const fluid::FluidRecipe& before, const fluid::FluidRecipe& after)
{
    if (before.kind != after.kind || before.seed != after.seed) return 0.0f;
    /// @note 使っていない側の設定 (気体のときの liquid) は解きにも 2D の描画にも効かない。
    const bool simulationChanged = after.kind == FluidKind::Gas ? GasDiffers(before.gas, after.gas)
                                                                : LiquidDiffers(before.liquid, after.liquid);
    if (simulationChanged || OutputDiffers(before.output, after.output)) return 0.0f;
    if (before.sources.size() != after.sources.size() || before.forces.size() != after.forces.size()
        || before.colliders.size() != after.colliders.size())
        return 0.0f;

    float first = kNever;
    for (std::size_t i = 0; i < after.sources.size(); ++i)
        first = (std::min)(first, PartInvalidation(before.sources[i], after.sources[i],
                                                   SourceDiffers(before.sources[i], after.sources[i])));
    for (std::size_t i = 0; i < after.forces.size(); ++i)
        first = (std::min)(first, PartInvalidation(before.forces[i], after.forces[i],
                                                   ForceDiffers(before.forces[i], after.forces[i])));
    for (std::size_t i = 0; i < after.colliders.size(); ++i)
        first = (std::min)(first, PartInvalidation(before.colliders[i], after.colliders[i],
                                                   ColliderDiffers(before.colliders[i], after.colliders[i])));
    if (first == kNever) return -1.0f;
    /// @note 部品の時刻はソルバーの時計 (warmup を含む)。プレビューの時計は warmup の後を 0 とする。
    const float warmup = std::clamp(after.output.warmup, 0.0f, kMaxWarmup);
    return (std::max)(first - warmup, 0.0f);
}

struct FluidPreviewCache::Impl {
    /// @note 最後に受け取ったレシピ (次の変更と比べる基準) と、焼きと同じ丸めをしたもの (解きに渡す)。
    fluid::FluidRecipe recipe;
    fluid::FluidRecipe preview;
    bool hasRecipe = false;
    std::uint64_t revision = 0;
    FluidPreviewQuality quality = FluidPreviewQuality::Draft;
    /// @note 呼び手が絵を出している一辺 [画面画素]。0 = まだ描いていない。
    float viewSide = 0.0f;

    std::vector<Slot> slots;
    float frameDt = 0.0f;
    /// @note 場 (格子の大きさ・コマ割り) が変わるたびに増える。古い世代の途中経過は続きを解く起点にできない。
    std::uint64_t simEpoch = 1;
    /// @note 部品が変わるたびに増える。古い世代でも、ReplaceOperators で部品を差し替えれば起点に使える。
    std::uint64_t partEpoch = 1;
    /// @note 走っている解きの結果を受け取ってよいか。取り消すたびに増える。
    std::uint64_t generation = 1;
    std::uint64_t nextVersion = 1;

    std::thread worker;
    std::shared_ptr<Channel> channel;
    std::uint64_t jobGeneration = 0;
    FluidShading jobShading = FluidShading::Smoke;

    /// @note TextureAt は引数にレンダラーを取らないので、最後の Tick で受け取ったものを使う。
    renderer::ResourceManager* resources = nullptr;
    renderer::IImGuiRenderer* imguiRenderer = nullptr;
    PreviewTexture texture;
    bool checkerBackground = true;
    std::vector<std::uint8_t> pixels;
    int uploadedIndex = -1;
    std::uint64_t uploadedVersion = 0;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    /// @note GPU 資源には触らない (Shutdown を呼ばずに壊されても、止めて待つだけで済むように)。
    ~Impl()
    {
        CancelJob();
        if (worker.joinable()) worker.join();
    }

    void CancelJob()
    {
        if (channel != nullptr) channel->cancel.store(true, std::memory_order_relaxed);
    }

    void Apply(Produced& item)
    {
        if (item.index < 0 || item.index >= static_cast<int>(slots.size())) return;
        Slot& slot = slots[static_cast<std::size_t>(item.index)];
        if (item.hasImage) {
            slot.image = std::move(item.image);
            slot.has = true;
            slot.stale = false;
            slot.shading = jobShading;
            slot.version = nextVersion++;
        }
        if (item.snapshot != nullptr) slot.snapshot = std::move(item.snapshot);
    }

    void Drain()
    {
        if (channel == nullptr) return;
        std::vector<Produced> produced = channel->Take();
        if (jobGeneration != generation) return;
        for (Produced& item : produced) Apply(item);
    }

    void ReapFinished()
    {
        if (!worker.joinable() || channel == nullptr || !channel->done.load(std::memory_order_acquire)) return;
        worker.join();
        Drain();
        channel.reset();
    }

    void StopWorker()
    {
        CancelJob();
        if (worker.joinable()) worker.join();
        Drain();
        channel.reset();
    }

    /// @note 取り消した解きは、次の Tick で終わったのを見届けてから次を出す (裏のスレッドは常に 1 本)。
    void Abandon()
    {
        Drain();
        CancelJob();
        ++generation;
    }

    [[nodiscard]] float FrameTime(std::size_t index) const { return static_cast<float>(index) * frameDt; }

    void ResetLayout(int frameCount, float dt)
    {
        Abandon();
        ++simEpoch;
        ++partEpoch;
        slots.assign(static_cast<std::size_t>(frameCount), Slot{});
        frameDt = dt;
        uploadedIndex = -1;
    }

    void Invalidate(float from, bool lookChanged)
    {
        const bool resolve = from >= 0.0f;
        if (!resolve && !lookChanged) return;
        Abandon();
        if (resolve) {
            /// @note 場はそのまま。残したコマの手前の途中経過は «部品を差し替えて続ける» 起点として生かす。
            ++partEpoch;
            /// @note 変わり目ちょうどのコマも捨てる (時計の積算誤差で変わった後の刻みを含んでいることがある)。
            for (std::size_t i = 0; i < slots.size(); ++i)
                if (FrameTime(i) >= from - kTimeEpsilon) slots[i] = Slot{};
        }
        if (lookChanged)
            for (Slot& slot : slots)
                if (slot.has) slot.stale = true;
    }

    [[nodiscard]] bool HasPendingWork() const
    {
        if (!hasRecipe) return false;
        return std::any_of(slots.begin(), slots.end(), [](const Slot& slot) { return !slot.has || slot.stale; });
    }

    /// @note 今のレシピ・品質・表示の大きさで描く画像の一辺。
    [[nodiscard]] int ImageSide() const
    {
        if (!hasRecipe) return 0;
        return ScaleFor(preview, quality, static_cast<int>(slots.size()), viewSide).image;
    }

    void StartNextJob()
    {
        if (!hasRecipe || slots.empty() || worker.joinable()) return;
        const int count = static_cast<int>(slots.size());
        const PreviewScale scale = ScaleFor(preview, quality, count, viewSide);

        /// @note 全コマ持てる予算なら毎コマ、超えるなら数コマおきに途中経過を持つ。
        const std::size_t total = EstimateSnapshotBytes(preview, scale.grid) * static_cast<std::size_t>(count);
        const int stride = total <= kSnapshotBudgetBytes
                               ? 1
                               : static_cast<int>((total + kSnapshotBudgetBytes - 1) / kSnapshotBudgetBytes);
        for (int i = 0; i < count; ++i)
            if (i % stride != 0) slots[static_cast<std::size_t>(i)].snapshot.reset();

        Job job;
        job.recipe = preview;
        job.kind = preview.kind;
        job.grid = scale.grid;
        job.imageSize = scale.image;
        job.frameDt = frameDt;
        job.substeps = preview.output.substeps;
        job.warmupFrames = fluid::FluidWarmupFrames(preview.output.warmup, frameDt);
        job.simEpoch = simEpoch;
        job.partEpoch = partEpoch;

        const auto needsSolve = [](const Slot& slot) { return !slot.has || (slot.stale && slot.snapshot == nullptr); };
        int first = -1;
        int last = -1;
        for (int i = 0; i < count; ++i) {
            const Slot& slot = slots[static_cast<std::size_t>(i)];
            if (slot.has && slot.stale && slot.snapshot != nullptr) job.rerender.emplace_back(i, slot.snapshot);
            if (needsSolve(slot)) {
                if (first < 0) first = i;
                last = i;
            }
        }
        if (job.rerender.empty() && first < 0) return;

        if (first >= 0) {
            /// @note 解き直しの起点は «変わり目の手前で一番新しい途中経過»。残したコマと同じ状態なので、
            /// @note       そこから続けても残したコマと食い違わない。
            for (int i = first - 1; i >= 0; --i) {
                const Slot& slot = slots[static_cast<std::size_t>(i)];
                if (slot.snapshot != nullptr && slot.snapshot->simEpoch == simEpoch) {
                    job.resumeFrom = slot.snapshot;
                    job.resumeIndex = i;
                    job.replaceOperators = slot.snapshot->partEpoch != partEpoch;
                    break;
                }
            }
            job.lastIndex = last;
            job.render.assign(static_cast<std::size_t>(count), 0);
            job.keepSnapshot.assign(static_cast<std::size_t>(count), 0);
            /// @note 起点より前にも印を付ける。続きを解くときは通らないが、差し替えを断られて頭から解き直す
            /// @note       ことになったら、通りがかりで古い途中経過を取り直せる。
            for (int i = 0; i <= last; ++i) {
                const Slot& slot = slots[static_cast<std::size_t>(i)];
                const bool current = slot.snapshot != nullptr && slot.snapshot->simEpoch == simEpoch
                                  && slot.snapshot->partEpoch == partEpoch;
                job.render[static_cast<std::size_t>(i)] = needsSolve(slot) ? 1 : 0;
                job.keepSnapshot[static_cast<std::size_t>(i)] = (i % stride == 0 && !current) ? 1 : 0;
            }
        }

        jobGeneration = generation;
        jobShading = EffectiveShading(preview);
        channel = std::make_shared<Channel>();
        worker = std::thread([shared = channel, job = std::move(job)]() { RunJob(job, *shared); });
    }

    [[nodiscard]] int FindFrame(float time) const
    {
        if (slots.empty() || !(frameDt > 0.0f)) return -1;
        const int count = static_cast<int>(slots.size());
        float position = time / frameDt + 1.0e-3f;
        if (!std::isfinite(position)) position = 0.0f;
        const int index = static_cast<int>(std::floor(std::clamp(position, 0.0f, static_cast<float>(count - 1))));
        for (int i = index; i >= 0; --i)
            if (slots[static_cast<std::size_t>(i)].has) return i;
        for (int i = index + 1; i < count; ++i)
            if (slots[static_cast<std::size_t>(i)].has) return i;
        return -1;
    }

    void ReleaseTexture(renderer::ResourceManager& manager)
    {
        /// @note デバイスリセット後の古いハンドルは既に無効。解放すると別の資源を消しかねない。
        if (texture.handle.IsValid() && texture.resetVersion == manager.GetResetVersion())
            manager.Release(texture.handle);
        texture = PreviewTexture{};
        uploadedIndex = -1;
    }

    [[nodiscard]] ImTextureID Upload(int index)
    {
        if (resources == nullptr || imguiRenderer == nullptr || texture.unsupported) return ImTextureID{};
        const Slot& slot = slots[static_cast<std::size_t>(index)];
        const asset::FluidFrameImage& frame = slot.image;
        const std::size_t pixelCount = static_cast<std::size_t>((std::max)(frame.size, 0))
                                     * static_cast<std::size_t>((std::max)(frame.size, 0));
        if (frame.size <= 0 || frame.rgba.size() < pixelCount * 4) return ImTextureID{};

        const std::uint64_t resetVersion = resources->GetResetVersion();
        if (!texture.handle.IsValid() || texture.size != frame.size || texture.resetVersion != resetVersion) {
            ReleaseTexture(*resources);
            const auto size = static_cast<std::uint32_t>(frame.size);
            texture.handle = resources->CreateDynamicTexture(size, size, renderer::DynamicTextureFormat::RGBA8);
            if (!texture.handle.IsValid()) {
                /// @note 未対応バックエンド。以降は作り直そうとせず、呼び手が FrameAt を矩形で描く。
                texture.unsupported = true;
                return ImTextureID{};
            }
            texture.size = frame.size;
            texture.resetVersion = resetVersion;
            texture.textureId = imguiRenderer->GetImTextureID(texture.handle, *resources);
        }
        if (texture.textureId == nullptr) return ImTextureID{};
        if (uploadedIndex == index && uploadedVersion == slot.version) return widgets::ToImTextureID(texture.textureId);

        renderer::ITexture* gpu = resources->Get(texture.handle);
        if (gpu == nullptr) return ImTextureID{};
        pixels.resize(pixelCount * 4);
        for (int y = 0; y < frame.size; ++y) {
            for (int x = 0; x < frame.size; ++x) {
                const std::size_t pixel = static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.size)
                                        + static_cast<std::size_t>(x);
                float color[3];
                CompositeFluidPreviewPixel(&frame.rgba[pixel * 4], x, y, frame.size, viewSide,
                                           slot.shading, checkerBackground, color);
                for (int c = 0; c < 3; ++c)
                    pixels[pixel * 4 + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(color[c] * 255.0f + 0.5f);
                pixels[pixel * 4 + 3] = 255;
            }
        }
        const auto size = static_cast<std::uint32_t>(frame.size);
        if (!gpu->UpdateRegion(0, 0, size, size, pixels.data(), size * 4)) return ImTextureID{};
        uploadedIndex = index;
        uploadedVersion = slot.version;
        return widgets::ToImTextureID(texture.textureId);
    }
};

FluidPreviewCache::FluidPreviewCache() : m_impl(std::make_unique<Impl>()) {}

FluidPreviewCache::~FluidPreviewCache() = default;

void FluidPreviewCache::SetRecipe(const fluid::FluidRecipe& recipe, std::uint64_t revision, float invalidateFrom)
{
    Impl& s = *m_impl;
    if (s.hasRecipe && revision == s.revision) return;

    /// @note 呼び手の申告と自前の比較の早い方を取る。呼び手はレシピに現れない変化 (発生源の画像ファイルの
    /// @note       書き換えなど) を知っていることがあり、自前の比較は申告の取りこぼしを拾う。
    float from = 0.0f;
    bool lookChanged = false;
    const bool first = !s.hasRecipe;
    if (!first) {
        from = CombineInvalidation(invalidateFrom, FluidInvalidationTime(s.recipe, recipe));
        lookChanged = LookDiffers(s.recipe, recipe);
    }
    s.recipe = recipe;
    s.preview = NormalizedForPreview(recipe);
    s.revision = revision;
    s.hasRecipe = true;

    const int count = FrameCountOf(s.preview);
    const float dt = FrameDtOf(s.preview);
    if (first || count != static_cast<int>(s.slots.size()) || dt != s.frameDt) {
        s.ResetLayout(count, dt);
        return;
    }
    s.Invalidate(from, lookChanged);
}

void FluidPreviewCache::SetQuality(FluidPreviewQuality quality)
{
    Impl& s = *m_impl;
    if (quality == s.quality) return;
    s.quality = quality;
    if (!s.hasRecipe) return;
    /// @note 格子が変わると途中経過は使えない (部品の差し替えでは直せない)。絵は解き直しが追いつくまで古いまま見せる。
    s.Abandon();
    ++s.simEpoch;
    ++s.partEpoch;
    for (Slot& slot : s.slots) {
        slot.snapshot.reset();
        if (slot.has) slot.stale = true;
    }
}

FluidPreviewQuality FluidPreviewCache::Quality() const
{
    return m_impl->quality;
}

void FluidPreviewCache::SetCheckerBackground(bool enabled)
{
    if (m_impl->checkerBackground == enabled) return;
    m_impl->checkerBackground = enabled;
    m_impl->uploadedIndex = -1;
}

void FluidPreviewCache::SetPreviewSide(float sidePixels)
{
    Impl& s = *m_impl;
    const float side = sidePixels > 0.0f ? (std::min)(sidePixels, kMaxViewSide) : 0.0f;
    if (side == s.viewSide) return;
    const int before = s.ImageSide();
    s.viewSide = side;
    if (!s.hasRecipe || s.ImageSide() == before) return;
    /// @note 格子は変わらないので途中経過はそのまま通じる。Look の変更と同じく «描き直し» で済み、
    /// @note       途中経過を捨てられたコマだけが解き直しになる。
    s.Abandon();
    for (Slot& slot : s.slots)
        if (slot.has) slot.stale = true;
}

void FluidPreviewCache::Tick(renderer::ResourceManager* resources, renderer::IImGuiRenderer* imguiRenderer)
{
    Impl& s = *m_impl;
    s.resources = resources;
    s.imguiRenderer = imguiRenderer;
    s.Drain();
    s.ReapFinished();
    s.StartNextJob();
}

const asset::FluidFrameImage* FluidPreviewCache::FrameAt(float time, fluid::FluidShading* shading) const
{
    const int index = m_impl->FindFrame(time);
    if (index < 0) return nullptr;
    const Slot& slot = m_impl->slots[static_cast<std::size_t>(index)];
    if (shading != nullptr) *shading = slot.shading;
    return &slot.image;
}

ImTextureID FluidPreviewCache::TextureAt(float time)
{
    const int index = m_impl->FindFrame(time);
    return index >= 0 ? m_impl->Upload(index) : ImTextureID{};
}

float FluidPreviewCache::SolvedUntil() const
{
    /// @note コマ i は [i·dt, (i+1)·dt) を受け持つ。全コマ解ければ Duration() に届く。
    std::size_t solved = 0;
    for (const Slot& slot : m_impl->slots) {
        if (!slot.has || slot.stale) break;
        ++solved;
    }
    return static_cast<float>(solved) * m_impl->frameDt;
}

float FluidPreviewCache::Duration() const
{
    return m_impl->hasRecipe ? m_impl->preview.output.duration : 0.0f;
}

int FluidPreviewCache::FrameCount() const
{
    return static_cast<int>(m_impl->slots.size());
}

float FluidPreviewCache::FrameDt() const
{
    return m_impl->frameDt;
}

bool FluidPreviewCache::IsSolving() const
{
    return m_impl->worker.joinable() || m_impl->HasPendingWork();
}

void FluidPreviewCache::Shutdown(renderer::ResourceManager* resources)
{
    Impl& s = *m_impl;
    s.StopWorker();
    if (resources != nullptr) s.ReleaseTexture(*resources);
    else                      s.texture = PreviewTexture{};
    s.resources = nullptr;
    s.imguiRenderer = nullptr;
    s.uploadedIndex = -1;
}

}
