/// @file    FluidBakeService.cpp
/// @brief   .fluid の焼きとプレビューを受け付けるエディター常駐のジョブ窓口
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include <Editor/Util/FluidBakeService.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/FlipbookInspector.hpp>
#include <Editor/Util/FluidAssetWriters.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/BakeFingerprint.hpp>
#include <Engine/Asset/FluidBakeBudget.hpp>
#include <Engine/Asset/FluidBaker.hpp>
#include <Engine/Asset/FluidRecipeCodec.hpp>
#include <Fluid/FluidSolver.hpp>
#include <Fluid/FluidStepping.hpp>
#include <Engine/Asset/FluidVolumeBake.hpp>
#include <Engine/Asset/VolumeFlipbookBaker.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <DirectXTex.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <numeric>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace fbzz::editor {
namespace {

constexpr std::size_t kFinishedJobsKept = 32;
/// @note 3D プレビューは CPU で解く液体だと 1 コマ目まで数十秒かかる。これを超えたら諦める。
constexpr float kVolumePreviewTimeoutSeconds = 180.0f;
/// @note 解けたのに読み戻せない (RT が作れていない) まま回り続けないよう、連続失敗の上限を置く。
constexpr int kVolumePreviewReadbackRetries = 4;

/// @note WIC は呼び出しスレッドで COM が初期化されている必要がある。自分が初期化した分だけ戻す。
/// @note メインスレッドの STA では RPC_E_CHANGED_MODE で素通りする。
class ComScope {
public:
    ComScope() : m_result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope()
    {
        if (SUCCEEDED(m_result)) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    HRESULT m_result;
};

bool SavePngRgba8(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                  std::vector<std::uint8_t>& pixels, std::string& outError)
{
    const ComScope com;
    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(), directoryError);
    DirectX::Image image{};
    image.width = width;
    image.height = height;
    image.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    image.rowPitch = static_cast<std::size_t>(width) * 4;
    image.slicePitch = image.rowPitch * height;
    image.pixels = pixels.data();
    if (FAILED(DirectX::SaveToWICFile(image, DirectX::WIC_FLAGS_NONE, GUID_ContainerFormatPng, path.c_str()))) {
        outError = "PNG を書き出せません: " + util::FileSystem::PathToUtf8(path);
        return false;
    }
    return true;
}

std::string PathKey(const std::string& path)
{
    std::string key = util::FileSystem::NormalizePathSeparators(path);
    for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return key;
}

std::string StemOf(const std::string& path)
{
    return util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(path).stem());
}

/// @brief 既存 GUID と材質の編集値を仮出力へ引き継ぐ。
bool PrepareFluidBakeStage(const std::filesystem::path& finalDirectory,
                           const std::filesystem::path& stageParent, const std::string& stem,
                           std::uint32_t id, bool includeDerived, const std::string& vfxPath,
                           std::filesystem::path& outStage, std::string& outError)
{
    std::error_code error;
    std::filesystem::create_directories(finalDirectory, error);
    if (error) { outError = "出力先を作れません: " + error.message(); return false; }
    std::filesystem::create_directories(stageParent, error);
    if (error) { outError = "仮出力先を作れません: " + error.message(); return false; }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    outStage = stageParent / (".fluid-bake-stage-" + std::to_string(id) + "-" + std::to_string(stamp));
    if (!std::filesystem::create_directories(outStage, error) || error) {
        outError = "仮出力先を作れません: " + error.message();
        return false;
    }
    static constexpr std::array<const char*, 19> suffixes{
        ".png.meta", ".dds.meta", "_mv.png.meta", "_mv.dds.meta",
        "_6wayP.png.meta", "_6wayP.dds.meta", "_6wayN.png.meta", "_6wayN.dds.meta",
        "_6wayC.png.meta", "_6wayC.dds.meta", "_6wayE.png.meta", "_6wayE.dds.meta",
        "_Flipbook.png.meta", "_Flipbook.dds.meta", "_MV.png.meta", "_MV.dds.meta",
        "_Velocity.png.meta", ".mat", ".mat.meta"
    };
    for (const char* suffix : suffixes) {
        if (!includeDerived && (std::string_view(suffix) == ".mat"
            || std::string_view(suffix) == ".mat.meta")) continue;
        const auto source = finalDirectory / (stem + suffix);
        if (!std::filesystem::exists(source, error)) { error.clear(); continue; }
        std::filesystem::copy_file(source, outStage / source.filename(),
                                   std::filesystem::copy_options::overwrite_existing, error);
        if (error) { outError = "既存のアセット設定を仮出力へ写せません: " + error.message(); return false; }
    }
    if (includeDerived && !vfxPath.empty()) {
        const auto vfx = util::FileSystem::PathFromUtf8(vfxPath);
        for (const auto& source : { vfx, std::filesystem::path(vfx.string() + ".meta") }) {
            if (!std::filesystem::exists(source, error)) { error.clear(); continue; }
            std::filesystem::copy_file(source, outStage / source.filename(),
                                       std::filesystem::copy_options::overwrite_existing, error);
            if (error) { outError = "既存の VFX 設定を仮出力へ写せません: " + error.message(); return false; }
        }
    }
    return true;
}

/// @brief 仮出力一式を公開し、途中の失敗では古いファイルを戻す。
bool PublishFluidBakeStage(const std::filesystem::path& stage,
                           const std::filesystem::path& finalDirectory,
                           const std::string& vfxPath, std::vector<std::string>& outPaths,
                           std::string& outError, bool& outRollbackIncomplete)
{
    outRollbackIncomplete = false;
    struct Entry {
        std::filesystem::path source;
        std::filesystem::path target;
        std::filesystem::path backup;
        bool backedUp = false;
        bool published = false;
    };
    std::vector<Entry> entries;
    std::error_code error;
    for (std::filesystem::directory_iterator it(stage, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file()) continue;
        const auto name = it->path().filename();
        const std::string file = util::FileSystem::PathToUtf8(name);
        if (file.ends_with(".meta")) {
            const auto assetFile = stage / util::FileSystem::PathFromUtf8(file.substr(0, file.size() - 5));
            if (!std::filesystem::exists(assetFile, error)) { error.clear(); continue; }
        }
        const bool isVfx = !vfxPath.empty() && (file == util::FileSystem::GetFilename(vfxPath)
            || file == util::FileSystem::GetFilename(vfxPath) + ".meta");
        const auto target = isVfx
            ? util::FileSystem::PathFromUtf8(vfxPath + (file.ends_with(".meta") ? ".meta" : ""))
            : finalDirectory / name;
        entries.push_back({ it->path(), target, stage / "backup" / name });
    }
    if (error) { outError = "仮出力を列挙できません: " + error.message(); return false; }
    const auto priority = [](const Entry& entry) {
        const std::string name = entry.target.filename().string();
        if (name.ends_with(".vfx.meta")) return 5;
        if (name.ends_with(".vfx")) return 4;
        if (name.ends_with(".mat.meta")) return 3;
        if (name.ends_with(".mat")) return 2;
        return name.ends_with(".meta") ? 1 : 0;
    };
    std::stable_sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
        return priority(a) < priority(b);
    });
    for (const Entry& entry : entries) {
        const bool exists = std::filesystem::exists(entry.target, error);
        if (error) break;
        if (exists && !std::filesystem::is_regular_file(entry.target, error)) {
            outError = "Bake 出力先がファイルではありません: "
                + util::FileSystem::PathToUtf8(entry.target);
            return false;
        }
        if (error) break;
    }
    if (error) { outError = "Bake 出力先を確認できません: " + error.message(); return false; }
    std::filesystem::create_directories(stage / "backup", error);
    if (error) { outError = "復旧用フォルダを作れません: " + error.message(); return false; }
    for (Entry& entry : entries) {
        std::filesystem::create_directories(entry.target.parent_path(), error);
        if (error) break;
        if (std::filesystem::exists(entry.target, error)) {
            std::filesystem::rename(entry.target, entry.backup, error);
            if (error) break;
            entry.backedUp = true;
        }
        std::filesystem::rename(entry.source, entry.target, error);
        if (error) break;
        entry.published = true;
        outPaths.push_back(util::FileSystem::PathToUtf8(entry.target));
    }
    if (!error) return true;
    outError = "Bake 出力を確定できません: " + error.message();
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        std::error_code rollback;
        if (it->published) std::filesystem::remove(it->target, rollback);
        if (it->backedUp) std::filesystem::rename(it->backup, it->target, rollback);
        if (rollback) {
            outRollbackIncomplete = true;
            outError += " / 復旧失敗: " + rollback.message();
        }
    }
    if (outRollbackIncomplete)
        outError += " / 復旧用ファイル: " + util::FileSystem::PathToUtf8(stage / "backup");
    outPaths.clear();
    return false;
}

void RemoveFluidBakeStage(std::filesystem::path stage)
{
    if (stage.empty()) return;
    std::thread([stage = std::move(stage)]() {
        std::error_code error;
        std::filesystem::remove_all(stage, error);
    }).detach();
}

/// @brief 仮 Bake の計算量を落とす。時間範囲は変えず、サンプルするコマ数だけ減らす。
void ApplyFluidDraftQuality(fluid::FluidRecipe& recipe)
{
    recipe.output.columns = (std::min)(recipe.output.columns, 4);
    recipe.output.rows = (std::min)(recipe.output.rows, 4);
    recipe.output.frameSize = (std::min)(recipe.output.frameSize, 128);
    recipe.output.supersampling = 1;
    recipe.output.motionVectors = false;
    recipe.output.vectorField = false;
    recipe.gas.resolution = (std::min)(fluid::ResolveGasResolution(recipe), 64);
    recipe.bake.volumeResolution = (std::min)(recipe.bake.volumeResolution, 64);
    recipe.bake.raySteps = (std::min)(recipe.bake.raySteps, 128);
    recipe.bake.shadowSteps = (std::min)(recipe.bake.shadowSteps, 8);
    recipe.bake.sixWayLightmaps = false;
    fluid::NormalizeFluidOutput(recipe.output);
}

struct PreviewOutcome {
    bool success = false;
    std::string message;
    std::string fingerprint;
    int frame = 0;
    std::uint32_t seed = 0;
};

fluid::FluidShading EffectiveShading(const fluid::FluidRecipe& recipe)
{
    if (recipe.kind == fluid::FluidKind::Liquid) return fluid::FluidShading::Liquid;
    return recipe.render.shading == fluid::FluidShading::Liquid ? fluid::FluidShading::Smoke : recipe.render.shading;
}

/// @note Inspector のプレビューと同じ式で市松の上へ合成した、不透明な 8bit の絵にする。
/// @note 透過のまま出さない: 読む側 (AI・画像ビューアー) ごとに下地が違い、同じ PNG が別の絵に見える。
std::vector<std::uint8_t> CompositeFrame(const asset::FluidFrameImage& frame, fluid::FluidShading shading)
{
    const bool premultiplied = asset::FluidShadingIsPremultiplied(shading);
    const bool additive = shading == fluid::FluidShading::Glow;
    const int size = frame.size;
    const int checker = (std::max)(size / 16, 1);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * size + x;
            const float* rgba = &frame.rgba[pixel * 4];
            const float background = (((x / checker) + (y / checker)) & 1) != 0 ? 0.22f : 0.12f;
            for (int c = 0; c < 3; ++c) {
                float value = 0.0f;
                if (premultiplied) value = rgba[c] + background * (1.0f - rgba[3]);
                else if (additive) value = background + rgba[c] * rgba[3];
                else               value = rgba[c] * rgba[3] + background * (1.0f - rgba[3]);
                pixels[pixel * 4 + c] = static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            pixels[pixel * 4 + 3] = 255;
        }
    }
    return pixels;
}

/// @brief 合成済みのタイルをシートの (originX, originY) へ貼る。
void BlitTile(const std::vector<std::uint8_t>& tile, int tileSize, int sheetWidth, int originX, int originY,
              std::vector<std::uint8_t>& sheet)
{
    for (int y = 0; y < tileSize; ++y) {
        const std::size_t source = static_cast<std::size_t>(y) * tileSize * 4;
        const std::size_t destination =
            (static_cast<std::size_t>(originY + y) * sheetWidth + originX) * 4;
        if (destination + static_cast<std::size_t>(tileSize) * 4 > sheet.size()) return;
        std::copy_n(tile.begin() + static_cast<std::ptrdiff_t>(source), static_cast<std::size_t>(tileSize) * 4,
                    sheet.begin() + static_cast<std::ptrdiff_t>(destination));
    }
}

/// @note 2D のプレビュー (別スレッド) は **焼きと同じ入口** (RenderFluidBakeFrames) でコマを解く。
/// @note 見えている絵は焼いたアトラスのそのコマと 1 画素まで同じ。
PreviewOutcome RunFlatPreview(const fluid::FluidRecipe& source, const FluidPreviewRequest& request,
                              const std::filesystem::path& pngPath, std::atomic<float>& progress,
                              const std::atomic<bool>& cancel)
{
    fluid::FluidRecipe recipe = source;
    if (request.seed != 0) recipe.seed = request.seed;
    const fluid::FluidStepPlan plan = fluid::MakeFluidStepPlan(recipe);
    const int frame = request.frame >= 0 ? std::clamp(request.frame, 0, plan.frameCount - 1)
                                         : plan.FrameOfTime(request.time);
    const int variants = std::clamp(request.variants, 1, 16);
    const bool seedSheet = request.contactSheet && variants > 1;
    const int sheetSize = std::clamp(request.size, 32, 2048);
    const fluid::FluidShading shading = EffectiveShading(recipe);

    int columns = 1;
    int rows = 1;
    if (seedSheet) {
        columns = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(variants))));
        rows = (variants + columns - 1) / columns;
    } else if (request.contactSheet) {
        columns = (std::max)(recipe.output.columns, 1);
        rows = (std::max)(recipe.output.rows, 1);
    }
    const int tile = std::clamp(sheetSize / (std::max)(columns, rows), 16, 1024);
    const int sheetWidth = tile * columns;
    const int sheetHeight = tile * rows;
    std::vector<std::uint8_t> sheet(static_cast<std::size_t>(sheetWidth) * sheetHeight * 4, 0);
    for (std::size_t i = 3; i < sheet.size(); i += 4) sheet[i] = 255;

    const auto blit = [&](const asset::FluidFrameImage& image, int index) {
        if (image.size <= 0) return;
        const std::vector<std::uint8_t> pixels = CompositeFrame(image, shading);
        BlitTile(pixels, image.size, sheetWidth, (index % columns) * tile, (index / columns) * tile, sheet);
    };

    if (seedSheet) {
        /// @note seed だけを振った試し。«ばらつきが欲しい» は乱数ではなく seed の並べ方で満たす —
/// @note 気に入った 1 枚の seed をそのまま .fluid へ書けば、同じ絵が何度でも焼ける。
        for (int i = 0; i < variants; ++i) {
            if (cancel.load(std::memory_order_relaxed)) return { false, "キャンセルしました" };
            fluid::FluidRecipe variant = recipe;
            variant.seed = recipe.seed + static_cast<std::uint32_t>(i);
            asset::FluidFrameImage image;
            std::atomic<float> inner{ 0.0f };
            if (!asset::RenderFluidBakeFrame(variant, frame, tile, image, &inner, &cancel))
                return { false, "プレビューを描けませんでした" };
            blit(image, i);
            progress.store(0.95f * static_cast<float>(i + 1) / static_cast<float>(variants),
                           std::memory_order_relaxed);
        }
    } else {
        std::vector<int> wanted;
        if (request.contactSheet) {
            wanted.resize(static_cast<std::size_t>(plan.frameCount));
            std::iota(wanted.begin(), wanted.end(), 0);
        } else {
            wanted.push_back(frame);
        }
        std::vector<asset::FluidFrameImage> images;
        if (!asset::RenderFluidBakeFrames(recipe, wanted, tile, images, &progress, &cancel))
            return { false, cancel.load(std::memory_order_relaxed) ? "キャンセルしました"
                                                                   : "プレビューを描けませんでした" };
        for (std::size_t i = 0; i < images.size(); ++i) blit(images[i], static_cast<int>(i));
    }

    std::string error;
    if (!SavePngRgba8(pngPath, static_cast<std::uint32_t>(sheetWidth), static_cast<std::uint32_t>(sheetHeight),
                      sheet, error))
        return { false, error };
    progress.store(1.0f, std::memory_order_relaxed);
    PreviewOutcome outcome;
    outcome.success = true;
    outcome.fingerprint = asset::BakeFingerprintOf(sheet);
    outcome.frame = frame;
    outcome.seed = recipe.seed;
    outcome.message = request.contactSheet
        ? (seedSheet ? "seed を振った試しを " + std::to_string(variants) + " 枚並べました"
                     : "全 " + std::to_string(plan.frameCount) + " コマを並べました")
        : "コマ " + std::to_string(frame) + " を書きました";
    return outcome;
}

} // namespace

struct FluidBakeService::Impl {
    struct Job {
        FluidJobStatus status;
/// @brief 同じ .fluid の重複を弾く鍵 (Analytic の 3D は空)。
        std::string key;
        std::string fluidAbs;
        std::string projectRoot;
        std::filesystem::path finalDirectory;
        std::filesystem::path stageDirectory;
        std::string outputStem;
        bool preserveStageOnFailure = false;
        fluid::FluidRecipe recipe;
        bool volume = false;
        FluidBakeRequest bakeRequest;
        std::vector<FluidEffectLayer> effectLayers;
        std::vector<std::string> effectFiles;
        std::vector<std::string> effectMaterials;
        std::string effectVfxPath;
        std::size_t effectIndex = 0;
        FluidPreviewRequest previewRequest;
        asset::VolumeFlipbookBakeSettings volumeSettings;

        /// @note shared_ptr にする: キャンセル直後に残る WIC 書き込み中も Job より長く生存する。
        std::shared_ptr<std::atomic<float>> progress = std::make_shared<std::atomic<float>>(0.0f);
        std::shared_ptr<asset::FluidBakeTiming> timing = std::make_shared<asset::FluidBakeTiming>();
        std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
        std::future<asset::FluidBakeResult> flatBake;
        std::future<PreviewOutcome> flatPreview;

        bool previewRecorded = false;
        int readbackMisses = 0;
        std::chrono::steady_clock::time_point started;
        std::chrono::steady_clock::time_point lastTimingTick;
        std::chrono::steady_clock::time_point lastCompletedTick;
        int lastCompletedFrames = 0;
    };

    struct Finished {
        FluidJobStatus status;
        bool hasFlat = false;
        asset::FluidBakeResult flat;
        bool hasVolume = false;
        asset::VolumeFlipbookBakeResult volume;
        asset::VolumeFlipbookBakeSettings volumeSettings;
    };

    /// @note キャンセル後の 2D worker は完了まで保持し、同じ .fluid の次の焼きと競合させない。
    struct Orphan {
        std::string key;
        std::filesystem::path stageDirectory;
        std::future<asset::FluidBakeResult> bake;
        std::future<PreviewOutcome> preview;
    };

    std::vector<std::unique_ptr<Job>> active;
    std::deque<Finished> finished;
    std::vector<Orphan> orphans;
    std::deque<std::string> reloadQueue;
    asset::VolumeFlipbookBaker baker;
    renderer::ResourceManager* bakerResources = nullptr;
    Job* volumeJob = nullptr;
/// @brief baker が今 Atlas を持っている 3D の焼きの id (TakeBakedFlipbook の持ち主)。
    std::uint32_t bakerResultJob = 0;
    std::uint32_t nextId = 1;
/// @brief 前のフレームに AllowVolumePreviewSwitch が呼ばれたか / そのとき渡した可否。
    bool previewSwitchAsked = false;
    bool previewSwitchAllowed = true;

    [[nodiscard]] bool KeyBusy(const std::string& key) const
    {
        if (key.empty()) return false;
        for (const auto& job : active) {
            if (job->key == key) return true;
            for (const auto& file : job->effectFiles)
                if (PathKey(file) == key) return true;
        }
        for (const Orphan& orphan : orphans)
            if (orphan.key == key) return true;
        return false;
    }

    [[nodiscard]] const Finished* FindFinished(std::uint32_t id) const
    {
        for (const Finished& record : finished)
            if (record.status.id == id) return &record;
        return nullptr;
    }

    Job& Add(std::unique_ptr<Job> job, FluidJobKind kind)
    {
        job->status.id = nextId++;
        if (nextId == 0) nextId = 1;
        job->status.kind = kind;
        job->status.state = FluidJobState::Queued;
        job->status.message = "待機中";
        active.push_back(std::move(job));
        return *active.back();
    }

    void Retire(Job& job, Finished record)
    {
        record.status = job.status;
        if (volumeJob == &job) volumeJob = nullptr;
        const auto it = std::find_if(active.begin(), active.end(),
                                     [&](const std::unique_ptr<Job>& entry) { return entry.get() == &job; });
        if (it != active.end()) active.erase(it);
        finished.push_back(std::move(record));
        while (finished.size() > kFinishedJobsKept) finished.pop_front();
    }

    void Fail(Job& job, std::string message)
    {
        job.status.state = FluidJobState::Failed;
        job.status.message = std::move(message);
        if (!job.preserveStageOnFailure) RemoveFluidBakeStage(job.stageDirectory);
        Retire(job, {});
    }

    bool PrepareStage(Job& job)
    {
        job.finalDirectory = job.volume
            ? util::FileSystem::PathFromUtf8(job.volumeSettings.outputDirectory)
            : util::FileSystem::PathFromUtf8(job.fluidAbs).parent_path();
        job.outputStem = job.volume ? job.volumeSettings.baseName : StemOf(job.fluidAbs);
        std::filesystem::path stageParent = job.finalDirectory;
        if (!job.projectRoot.empty()) {
            const auto assets = util::FileSystem::PathFromUtf8(job.projectRoot) / "Assets";
            if (util::FileSystem::IsChildPathText(util::FileSystem::PathToUtf8(job.finalDirectory),
                                                   util::FileSystem::PathToUtf8(assets)))
                stageParent = util::FileSystem::PathFromUtf8(job.projectRoot) / "Library" / "FluidBakeStage";
        }
        std::string error;
        if (!PrepareFluidBakeStage(job.finalDirectory, stageParent, job.outputStem, job.status.id,
                                   !job.status.draft && !job.fluidAbs.empty(), job.bakeRequest.vfxPath,
                                   job.stageDirectory, error)) {
            Fail(job, error);
            return false;
        }
        return true;
    }

    [[nodiscard]] std::string FinalOutputPath(const Job& job, const std::string& staged) const
    {
        if (staged.empty()) return {};
        return util::FileSystem::PathToUtf8(job.finalDirectory
            / util::FileSystem::PathFromUtf8(staged).filename());
    }

    bool PublishStage(Job& job, std::string& outError)
    {
        std::vector<std::string> files;
        bool rollbackIncomplete = false;
        if (!PublishFluidBakeStage(job.stageDirectory, job.finalDirectory,
                                   job.bakeRequest.vfxPath, files, outError,
                                   rollbackIncomplete)) {
            job.preserveStageOnFailure = rollbackIncomplete;
            return false;
        }
        for (const std::string& file : files) {
            if (std::filesystem::path(file).extension() == ".meta") continue;
            job.status.outputs.push_back(file);
            if (!job.status.draft) {
                (void)asset::AssetDatabase::GuidFromPath(file);
                reloadQueue.push_back(file);
            }
        }
        RemoveFluidBakeStage(std::move(job.stageDirectory));
        job.stageDirectory.clear();
        return true;
    }

    /// @brief 焼いた .fluid の隣の .mat と、頼まれていれば 1 層の .vfx を書く。
    bool WriteDerivedAssets(Job& job, const FluidMaterialSource& source, float lifetime, EditorContext& ctx)
    {
        const std::string sibling = SiblingMaterialPath(job.fluidAbs);
        const std::filesystem::path stagedMaterial = job.stageDirectory
            / util::FileSystem::PathFromUtf8(sibling).filename();
        bool created = false;
        std::string error;
        if (WriteFluidParticleMaterial(util::FileSystem::PathToUtf8(stagedMaterial), source, created, error,
                                       false)) {
            if (asset::AssetDatabase::EnsureGuidMetaUnindexed(
                    util::FileSystem::PathToUtf8(stagedMaterial)).empty()) {
                job.status.message += "\n.mat の GUID を仮出力へ保存できません";
                return false;
            }
            job.status.materialPath = sibling;
            job.status.message += created ? "\n.mat を作りました: " : "\n.mat を更新しました: ";
            job.status.message += NormalizeAssetPath(sibling);
        } else {
            job.status.message += "\n" + error;
            return false;
        }
        if (!job.bakeRequest.vfxPath.empty()) {
            const std::filesystem::path vfx = util::FileSystem::PathFromUtf8(job.bakeRequest.vfxPath);
            const std::string rootName = job.bakeRequest.vfxRootName.empty() ? StemOf(job.fluidAbs)
                                                                              : job.bakeRequest.vfxRootName;
            const std::filesystem::path stagedVfx = job.stageDirectory / vfx.filename();
            if (WriteSingleEmitterVfx(stagedVfx, rootName,
                                      NormalizeAssetPath(job.status.materialPath), lifetime)) {
                if (asset::AssetDatabase::EnsureGuidMetaUnindexed(
                        util::FileSystem::PathToUtf8(stagedVfx)).empty()) {
                    job.status.message += "\n.vfx の GUID を仮出力へ保存できません";
                    return false;
                }
                job.status.vfxPath = util::FileSystem::PathToUtf8(vfx);
                job.status.message += "\n.vfx を書きました: " + NormalizeAssetPath(job.status.vfxPath);
            } else {
                job.status.message += "\n.vfx を書き出せません: " + job.bakeRequest.vfxPath;
                return false;
            }
        }
        (void)ctx;
        return true;
    }

    void FinishFlatBake(Job& job, EditorContext& ctx)
    {
        asset::FluidBakeResult result = job.flatBake.get();
        job.status.simulationSeconds = job.timing->simulationSeconds.load(std::memory_order_relaxed);
        job.status.renderSeconds = job.timing->renderSeconds.load(std::memory_order_relaxed);
        job.status.outputSeconds = job.timing->outputSeconds.load(std::memory_order_relaxed);
        job.status.slowestSimulationFrameSeconds = job.timing->slowestSimulationFrameSeconds.load(std::memory_order_relaxed);
        job.status.slowestSimulationFrame = job.timing->slowestSimulationFrame.load(std::memory_order_relaxed);
        job.status.slowestRenderFrameSeconds = job.timing->slowestRenderFrameSeconds.load(std::memory_order_relaxed);
        job.status.slowestRenderFrame = job.timing->slowestRenderFrame.load(std::memory_order_relaxed);
        job.status.elapsedSeconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - job.started).count();
        job.status.remainingSeconds = 0.0f;
        job.status.stage = 2;
        Finished record;
        record.hasFlat = true;
        job.status.progress = 1.0f;
        job.status.message = result.message;
        job.status.fingerprint = result.fingerprint;
        job.status.solverUsed = result.solverUsed;
        job.status.fallbackReason = result.fallbackReason;
        job.status.seed = job.recipe.seed;
        if (!result.success) {
            job.status.state = FluidJobState::Failed;
            RemoveFluidBakeStage(std::move(job.stageDirectory));
            record.flat = result;
            Retire(job, std::move(record));
            return;
        }
        if (!job.status.draft) {
            if (!WriteDerivedAssets(job, FluidMaterialSource::FromFlat(result),
                                    (std::max)(job.recipe.output.duration, 0.1f), ctx)) {
                Fail(job, job.status.message);
                return;
            }
        }
        std::string publishError;
        if (!PublishStage(job, publishError)) { Fail(job, publishError); return; }
        result.albedoPath = FinalOutputPath(job, result.albedoPath);
        result.motionVectorPath = FinalOutputPath(job, result.motionVectorPath);
        result.vectorFieldPath = FinalOutputPath(job, result.vectorFieldPath);
        record.flat = result;
        if (job.status.draft) {
            job.status.state = FluidJobState::Done;
            Retire(job, std::move(record));
            return;
        }
        InvalidateFlipbookAtlasPreview();
        asset::AssetManager::FlushFailed();
        ctx.requestAssetBrowserRefresh = true;
        if (!job.effectLayers.empty()) {
            if (job.status.materialPath.empty()) {
                Fail(job, "テンプレートの素材を保存できません: " + job.status.message);
                return;
            }
            job.effectMaterials.push_back(job.status.materialPath);
            ++job.effectIndex;
            if (job.effectIndex < job.effectLayers.size()) {
                job.fluidAbs = job.effectFiles[job.effectIndex];
                job.recipe = job.effectLayers[job.effectIndex].recipe;
                job.bakeRequest.fluidPath = job.fluidAbs;
                job.status.materialPath.clear();
                job.status.state = FluidJobState::Queued;
                job.progress->store(0.0f, std::memory_order_relaxed);
                job.status.progress = static_cast<float>(job.effectIndex) / static_cast<float>(job.effectLayers.size());
                return;
            }
            std::string error;
            const auto file = util::FileSystem::PathFromUtf8(job.effectVfxPath);
            if (!WriteLayeredFluidVfx(file, StemOf(job.effectVfxPath), job.effectLayers, job.effectMaterials, error)) {
                Fail(job, error);
                return;
            }
            job.status.vfxPath = job.effectVfxPath;
            job.status.outputs.push_back(job.effectVfxPath);
            job.status.message = "演出テンプレートを作成しました: " + NormalizeAssetPath(job.effectVfxPath);
            ctx.requestAssetBrowserRefresh = true;
        }
        job.status.state = FluidJobState::Done;
        Retire(job, std::move(record));
    }

    void FinishVolumeBake(Job& job, EditorContext& ctx)
    {
        job.status.elapsedSeconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - job.started).count();
        job.status.remainingSeconds = 0.0f;
        asset::VolumeFlipbookBakeResult result = baker.Result();
        Finished record;
        record.hasVolume = true;
        record.volumeSettings = job.volumeSettings;
        job.status.message = result.message;
        job.status.fingerprint = result.fingerprint;
        job.status.solverUsed = result.solverUsed;
        job.status.fallbackReason = result.fallbackReason;
        job.status.seed = job.recipe.seed;
        if (baker.State() == asset::VolumeFlipbookBakeState::Idle) {
            job.status.state = FluidJobState::Cancelled;
            RemoveFluidBakeStage(std::move(job.stageDirectory));
            Retire(job, std::move(record));
            return;
        }
        if (!result.success) {
            job.status.state = FluidJobState::Failed;
            RemoveFluidBakeStage(std::move(job.stageDirectory));
            record.volume = result;
            Retire(job, std::move(record));
            return;
        }
        job.status.colorEncodeSeconds = result.colorEncodeSeconds;
        job.status.motionEncodeSeconds = result.motionEncodeSeconds;
        job.status.sixWayEncodeSeconds = result.sixWayEncodeSeconds;
        if (!job.status.draft && !job.fluidAbs.empty()) {
            const float lifetime = (std::max)(static_cast<float>(result.frameCount) * job.volumeSettings.source.frameDt,
                                              0.1f);
            if (!WriteDerivedAssets(job, FluidMaterialSource::FromVolume(result, job.volumeSettings), lifetime, ctx)) {
                Fail(job, job.status.message);
                return;
            }
        }
        std::string publishError;
        if (!PublishStage(job, publishError)) { Fail(job, publishError); return; }
        for (std::string* path : { &result.colorPath, &result.motionPath, &result.sixWayPositivePath,
                                   &result.sixWayNegativePath, &result.sixWayAlbedoColorPath,
                                   &result.sixWayEmissionColorPath })
            *path = FinalOutputPath(job, *path);
        record.volume = result;
        bakerResultJob = job.status.id;
        job.status.progress = 1.0f;
        if (!job.status.draft) {
            asset::AssetManager::FlushFailed();
            ctx.requestAssetBrowserRefresh = true;
        }
        job.status.state = FluidJobState::Done;
        Retire(job, std::move(record));
    }

    void StartVolumeJob(Job& job, EditorContext& ctx)
    {
        job.started = std::chrono::steady_clock::now();
        job.lastTimingTick = job.started;
        job.lastCompletedTick = job.started;
        job.lastCompletedFrames = 0;
        bakerResources = ctx.resources;
        if (job.status.kind == FluidJobKind::Preview) {
            job.status.state = FluidJobState::Running;
            job.status.message = "プレビューを解いています";
            volumeJob = &job;
            return;
        }
        if (!PrepareStage(job)) return;
        asset::VolumeFlipbookBakeSettings working = job.volumeSettings;
        working.outputDirectory = util::FileSystem::PathToUtf8(job.stageDirectory);
        std::string error;
        if (!baker.Begin(working, *ctx.resources, error)) {
            Fail(job, error);
            return;
        }
        /// @note Begin で baker の前の Atlas は捨てられる。
        bakerResultJob = 0;
        job.status.state = FluidJobState::Running;
        job.status.message = "焼いています";
        volumeJob = &job;
    }

    void TickVolumeBake(Job& job, EditorContext& ctx)
    {
        const auto now = std::chrono::steady_clock::now();
        const float sinceLast = std::chrono::duration<float>(now - job.lastTimingTick).count();
        job.lastTimingTick = now;
        switch (baker.State()) {
        case asset::VolumeFlipbookBakeState::Recording:
            job.status.simulationSeconds += sinceLast;
            job.status.stage = 0;
            break;
        case asset::VolumeFlipbookBakeState::AwaitingCapture:
            job.status.renderSeconds += sinceLast;
            job.status.stage = 1;
            break;
        case asset::VolumeFlipbookBakeState::Encoding:
            job.status.outputSeconds += sinceLast;
            job.status.stage = 2;
            break;
        default: break;
        }
        baker.Tick(*ctx.renderer, *ctx.resources);
        const int completed = baker.CompletedFrames();
        if (completed > job.lastCompletedFrames) {
            const auto completedAt = std::chrono::steady_clock::now();
            const float frameSeconds = std::chrono::duration<float>(completedAt - job.lastCompletedTick).count()
                / static_cast<float>(completed - job.lastCompletedFrames);
            if (frameSeconds > job.status.slowestRenderFrameSeconds) {
                job.status.slowestRenderFrameSeconds = frameSeconds;
                job.status.slowestRenderFrame = completed - 1;
            }
            job.lastCompletedFrames = completed;
            job.lastCompletedTick = completedAt;
        }
        job.status.elapsedSeconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - job.started).count();
        const int total = (std::max)(baker.TotalFrames(), 1);
        job.status.progress = std::clamp(static_cast<float>(baker.CompletedFrames()) / static_cast<float>(total),
                                          0.0f, 1.0f);
        job.status.remainingSeconds = job.status.progress > 0.02f && job.status.progress < 0.99f
            ? job.status.elapsedSeconds * (1.0f - job.status.progress) / job.status.progress : -1.0f;
        if (baker.State() == asset::VolumeFlipbookBakeState::Encoding) {
            job.status.state = FluidJobState::Encoding;
            job.status.message = "書き出しています (BC7 圧縮)";
        }
        if (!baker.IsBusy()) FinishVolumeBake(job, ctx);
    }

    /// @note RecordPreview で描いた次のフレームで読み戻す (理由は VolumeFlipbookBaker.hpp を参照)。
    void TickVolumePreview(Job& job, EditorContext& ctx)
    {
        const float elapsed =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - job.started).count();
        if (job.previewRecorded && !baker.PreviewPending()) {
            std::vector<std::uint8_t> pixels;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            if (baker.ReadbackPreview(*ctx.renderer, *ctx.resources, pixels, width, height)) {
                for (std::size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
                std::string error;
                const std::filesystem::path png = util::FileSystem::PathFromUtf8(job.status.previewPngPath);
                if (!SavePngRgba8(png, width, height, pixels, error)) {
                    Fail(job, error);
                    return;
                }
                job.status.progress = 1.0f;
                job.status.state = FluidJobState::Done;
                job.status.fingerprint = asset::BakeFingerprintOf(pixels);
                job.status.solverUsed = baker.PreviewUsesGpu() ? "gpu" : "cpu";
                job.status.fallbackReason = baker.PreviewNoteIsFailure() ? std::string{} : baker.PreviewNote();
                job.status.message = "コマ " + std::to_string(job.previewRequest.frame) + " を書きました";
                Retire(job, {});
                return;
            }
            if (++job.readbackMisses > kVolumePreviewReadbackRetries) {
                const std::string& reason = baker.Result().message;
                Fail(job, reason.empty() ? std::string("プレビューを読み戻せません") : reason);
                return;
            }
        }
        if (elapsed > kVolumePreviewTimeoutSeconds) {
            Fail(job, "プレビューが時間内に解けませんでした");
            return;
        }
        asset::VolumePreviewOptions options;
        options.view = asset::VolumePreviewView::Color;
        options.background = asset::VolumePreviewBackground::Dark;
        baker.RecordPreview(*ctx.renderer, *ctx.resources, job.volumeSettings, job.previewRequest.time, options);
        job.previewRecorded = true;
        job.status.progress = baker.PreviewPending() ? 0.5f : 0.9f;
    }

    void StartFlatJob(Job& job)
    {
        job.started = std::chrono::steady_clock::now();
        job.lastTimingTick = job.started;
        job.status.state = FluidJobState::Running;
        const fluid::FluidRecipe recipe = job.recipe;
        std::shared_ptr<std::atomic<float>> progress = job.progress;
        std::shared_ptr<asset::FluidBakeTiming> timing = job.timing;
        std::shared_ptr<std::atomic<bool>> cancel = job.cancel;
        if (job.status.kind == FluidJobKind::Bake) {
            if (!PrepareStage(job)) return;
            const std::filesystem::path base = job.stageDirectory / job.outputStem;
            const std::string basePath = util::FileSystem::PathToUtf8(base);
            job.status.message = job.effectLayers.empty() ? "焼いています"
                : "素材 " + std::to_string(job.effectIndex + 1) + "/" + std::to_string(job.effectLayers.size())
                  + ": " + job.effectLayers[job.effectIndex].name;
            job.flatBake = std::async(std::launch::async, [recipe, basePath, progress, timing, cancel]() {
                return asset::BakeFluid(recipe, basePath, progress.get(), timing.get(), cancel.get());
            });
            return;
        }
        const FluidPreviewRequest request = job.previewRequest;
        const std::filesystem::path png = util::FileSystem::PathFromUtf8(job.status.previewPngPath);
        job.status.message = "プレビューを解いています";
        job.flatPreview = std::async(std::launch::async, [recipe, request, png, progress, cancel]() {
            return RunFlatPreview(recipe, request, png, *progress, *cancel);
        });
    }

    /// @brief 読み込み前の検証。成功したら outAbs / outRecipe を埋める。
    [[nodiscard]] bool ResolveFluid(const EditorContext& ctx, const std::string& path, std::string& outAbs,
                                    fluid::FluidRecipe& outRecipe, FluidJobError& outError) const
    {
        std::string abs = path.empty() ? std::string{} : asset::AssetManager::ResolveAssetPath(path);
        if ((abs.empty() || !util::FileSystem::Exists(abs)) && !path.empty()) {
            if (util::FileSystem::Exists(path))
                abs = path;
            else if (!ctx.projectRoot.empty())
                abs = ToProjectAssetDiskPath(ctx.projectRoot, path);
        }
        if (abs.empty() || !util::FileSystem::Exists(abs)) {
            outError = { "FLUID_NOT_FOUND", ".fluid が見つかりません: " + path };
            return false;
        }
        std::string loadError;
        if (!asset::LoadFluidRecipe(abs, outRecipe, &loadError)) {
            outError = { "FLUID_READ_FAILED", ".fluid を読み込めません: " + abs
                             + (loadError.empty() ? std::string{} : " (" + loadError + ")") };
            return false;
        }
        if (KeyBusy(PathKey(abs))) {
            outError = { "FLUID_BUSY", "この .fluid は焼いている / 焼く予定です: " + abs };
            return false;
        }
        outAbs = util::FileSystem::NormalizePathSeparators(abs);
        return true;
    }
};

FluidBakeService::FluidBakeService()
    : m_impl(std::make_unique<Impl>())
{
}

FluidBakeService::~FluidBakeService() = default;

std::uint32_t FluidBakeService::EnqueueBake(const EditorContext& ctx, const FluidBakeRequest& request,
                                             FluidJobError& outError)
{
    std::string abs;
    fluid::FluidRecipe recipe;
    if (!m_impl->ResolveFluid(ctx, request.fluidPath, abs, recipe, outError)) return 0;
    if (request.draft && request.recipeOverride) recipe = *request.recipeOverride;
    const bool volume = recipe.bake.mode == fluid::FluidBakeMode::Volume3D;
    if (volume && (ctx.renderer == nullptr || ctx.resources == nullptr)) {
        outError = { "NO_RENDERER", "3D の焼きにはレンダラーが要ります" };
        return 0;
    }
    /// @note 3D は Baker がパスから .fluid を読み直すので、メモリ上の seed 差し替えは効かない。
    /// @note 黙って «指定と違う seed» で焼くより、できないと言う。
    if (volume && request.seed != 0) {
        outError = { "BAD_ARG", "3D の焼きは seed の差し替えに対応していません (.fluid の seed を変えてください)" };
        return 0;
    }
    auto job = std::make_unique<Impl::Job>();
    job->key = PathKey(abs);
    job->fluidAbs = abs;
    job->projectRoot = ctx.projectRoot;
    job->recipe = recipe;
    if (request.seed != 0) job->recipe.seed = request.seed;
    if (request.draft) {
        if (ctx.projectRoot.empty()) {
            outError = { "BAD_PATH", "仮 Bake にはプロジェクトが要ります" };
            return 0;
        }
        ApplyFluidDraftQuality(job->recipe);
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::size_t sourceHash = std::hash<std::string>{}(job->key);
        const std::filesystem::path draft = util::FileSystem::PathFromUtf8(ctx.projectRoot)
            / "Library" / "FluidDraft" / util::FileSystem::PathFromUtf8(
                StemOf(abs) + "_" + std::to_string(sourceHash) + "_" + std::to_string(stamp) + ".fluid");
        const std::string draftPath = util::FileSystem::PathToUtf8(draft);
        std::error_code directoryError;
        std::filesystem::create_directories(draft.parent_path(), directoryError);
        if (directoryError || !asset::SaveFluidRecipe(draftPath, job->recipe)) {
            outError = { "WRITE_FAILED", "仮 Bake のレシピを書けません: " + draftPath };
            return 0;
        }
        job->fluidAbs = draftPath;
        job->status.draftFluidPath = draftPath;
        job->status.draft = true;
    }
    job->volume = volume;
    job->bakeRequest = request;
    job->status.fluidPath = abs;
    job->status.seed = job->recipe.seed;
    if (volume) job->volumeSettings = asset::MakeVolumeBakeSettings(job->recipe, job->fluidAbs);
    const asset::FluidBakeBudget budget = volume
        ? asset::EstimateVolumeBakeBudget(job->volumeSettings)
        : asset::EstimateFluidBakeBudget(job->recipe);
    std::string budgetError;
    if (!asset::ValidateFluidBakeBudget(budget,
             util::FileSystem::PathFromUtf8(job->fluidAbs).parent_path(), budgetError)) {
        outError = { "FLUID_BUDGET", budgetError };
        return 0;
    }
    return m_impl->Add(std::move(job), FluidJobKind::Bake).status.id;
}

std::uint32_t FluidBakeService::EnqueueEffectTemplate(const EditorContext& ctx, FluidEffectTemplate preset,
    const std::string& directory, FluidJobError& outError)
{
    auto layers = MakeFluidEffectLayers(preset);
    if (layers.empty() || ctx.projectRoot.empty() || directory.empty()) {
        outError = { "BAD_ARG", "テンプレートと作成先を指定してください" };
        return 0;
    }
    std::error_code error;
    const auto assets = std::filesystem::weakly_canonical(
        util::FileSystem::PathFromUtf8(ctx.projectRoot) / "Assets", error);
    if (error) { outError = { "BAD_PATH", "Assets の場所を解決できません" }; return 0; }
    auto folder = util::FileSystem::PathFromUtf8(directory);
    if (!folder.is_absolute()) folder = util::FileSystem::PathFromUtf8(ctx.projectRoot) / folder;
    folder = std::filesystem::weakly_canonical(folder, error);
    if (error || !util::FileSystem::IsChildPathText(util::FileSystem::PathToUtf8(folder),
                                                   util::FileSystem::PathToUtf8(assets))) {
        outError = { "BAD_PATH", "作成先はプロジェクトの Assets 内にしてください" };
        return 0;
    }
    if (std::filesystem::exists(folder, error) || error) {
        outError = { "ALREADY_EXISTS", "既存フォルダは変更しません。新しいフォルダ名を指定してください" };
        return 0;
    }
    if (!std::filesystem::create_directories(folder, error) || error) {
        outError = { "WRITE_FAILED", "作成先フォルダを作れません" };
        return 0;
    }
    auto job = std::make_unique<Impl::Job>();
    job->effectLayers = std::move(layers);
    job->projectRoot = ctx.projectRoot;
    job->effectVfxPath = util::FileSystem::PathToUtf8(folder / (folder.filename().wstring() + L".vfx"));
    job->key = PathKey(job->effectVfxPath);
    for (const auto& layer : job->effectLayers) {
        const auto file = folder / util::FileSystem::PathFromUtf8(layer.name + ".fluid");
        const std::string path = util::FileSystem::PathToUtf8(file);
        if (!asset::SaveFluidRecipe(path, layer.recipe)) {
            outError = { "WRITE_FAILED", "素材レシピを書けません: " + path + " (作成済みの素材は残します)" };
            return 0;
        }
        (void)asset::AssetDatabase::GuidFromPath(path);
        job->effectFiles.push_back(path);
        job->status.outputs.push_back(path);
    }
    job->fluidAbs = job->effectFiles.front();
    job->status.fluidPath = job->fluidAbs;
    job->recipe = job->effectLayers.front().recipe;
    job->bakeRequest.fluidPath = job->fluidAbs;
    const auto id = m_impl->Add(std::move(job), FluidJobKind::Bake).status.id;
    outError = {};
    return id;
}

std::uint32_t FluidBakeService::EnqueuePreview(const EditorContext& ctx, const FluidPreviewRequest& request,
                                               FluidJobError& outError)
{
    std::string abs;
    fluid::FluidRecipe recipe;
    if (!m_impl->ResolveFluid(ctx, request.fluidPath, abs, recipe, outError)) return 0;
    const bool volume = recipe.bake.mode == fluid::FluidBakeMode::Volume3D;
    if (volume && (ctx.renderer == nullptr || ctx.resources == nullptr)) {
        outError = { "NO_RENDERER", "3D のプレビューにはレンダラーが要ります" };
        return 0;
    }
    if (volume && (request.contactSheet || request.variants > 1)) {
        outError = { "BAD_ARG",
                     "3D (bake.mode = 3d) のプレビューはコンタクトシートと variants に対応していません" };
        return 0;
    }
    if (volume && request.seed != 0) {
        outError = { "BAD_ARG", "3D のプレビューは seed の差し替えに対応していません (.fluid の seed を変えてください)" };
        return 0;
    }
    auto job = std::make_unique<Impl::Job>();
    job->key = PathKey(abs);
    job->fluidAbs = abs;
    job->projectRoot = ctx.projectRoot;
    job->recipe = recipe;
    if (request.seed != 0) job->recipe.seed = request.seed;
    job->volume = volume;
    job->previewRequest = request;
    job->previewRequest.size = std::clamp(request.size, 32, 2048);
    /// @note 見るコマはここで決めてしまう。2D も 3D も «焼きの第 n コマ» を指す 1 つの番号で話す。
    const fluid::FluidStepPlan plan = fluid::MakeFluidStepPlan(job->recipe);
    job->previewRequest.frame = request.frame >= 0 ? std::clamp(request.frame, 0, plan.frameCount - 1)
                                                   : plan.FrameOfTime(request.time);
    job->previewRequest.time = plan.TimeOfFrame(job->previewRequest.frame);
    job->status.fluidPath = abs;
    job->status.previewFrame = job->previewRequest.frame;
    job->status.seed = job->recipe.seed;
    if (volume) {
        job->volumeSettings = asset::MakeVolumeBakeSettings(job->recipe, abs);
        job->volumeSettings.tileSize = std::clamp(job->previewRequest.size, 32, 1024);
    }
    Impl::Job& added = m_impl->Add(std::move(job), FluidJobKind::Preview);
    const std::string root = ctx.projectRoot.empty() ? std::string(".") : ctx.projectRoot;
    const std::filesystem::path png = util::FileSystem::PathFromUtf8(root) / "Library" / "FluidPreview"
        / util::FileSystem::PathFromUtf8(StemOf(abs) + "_" + std::to_string(added.status.id) + ".png");
    added.status.previewPngPath = util::FileSystem::NormalizePathSeparators(util::FileSystem::PathToUtf8(png));
    return added.status.id;
}

std::uint32_t FluidBakeService::EnqueueVolumeBake(const EditorContext& ctx,
                                                  const asset::VolumeFlipbookBakeSettings& settings,
                                                  FluidJobError& outError)
{
    if (ctx.renderer == nullptr || ctx.resources == nullptr) {
        outError = { "NO_RENDERER", "3D の焼きにはレンダラーが要ります" };
        return 0;
    }
    auto job = std::make_unique<Impl::Job>();
    job->volume = true;
    job->projectRoot = ctx.projectRoot;
    job->volumeSettings = settings;
    return m_impl->Add(std::move(job), FluidJobKind::Bake).status.id;
}

bool FluidBakeService::Cancel(std::uint32_t id)
{
    Impl& impl = *m_impl;
    const auto it = std::find_if(impl.active.begin(), impl.active.end(),
                                 [id](const std::unique_ptr<Impl::Job>& job) { return job->status.id == id; });
    if (it == impl.active.end()) return false;
    Impl::Job& job = **it;
    job.cancel->store(true, std::memory_order_relaxed);
    if (job.status.state != FluidJobState::Queued) {
        if (impl.volumeJob == &job && job.status.kind == FluidJobKind::Bake) {
            const bool encoding = impl.baker.State() == asset::VolumeFlipbookBakeState::Encoding
                || impl.baker.State() == asset::VolumeFlipbookBakeState::Cancelling;
            impl.baker.Cancel();
            if (encoding) {
                job.status.state = FluidJobState::Encoding;
                job.status.message = "書き出しの中断を待っています";
                return true;
            }
            RemoveFluidBakeStage(std::move(job.stageDirectory));
        }
        if (job.flatBake.valid() || job.flatPreview.valid()) {
            /// @note 進行中の WIC 書き込みだけは中断できない。終われば仮出力を破棄する。
            impl.orphans.push_back({ job.effectLayers.empty() ? job.key : PathKey(job.fluidAbs),
                                     std::move(job.stageDirectory),
                                     std::move(job.flatBake), std::move(job.flatPreview) });
        }
    }
    job.status.state = FluidJobState::Cancelled;
    job.status.message = "キャンセルしました";
    impl.Retire(job, {});
    return true;
}

const FluidJobStatus* FluidBakeService::Find(std::uint32_t id) const
{
    for (const auto& job : m_impl->active)
        if (job->status.id == id) return &job->status;
    const Impl::Finished* record = m_impl->FindFinished(id);
    return record != nullptr ? &record->status : nullptr;
}

bool FluidBakeService::IsBusy(const std::string& fluidPath) const
{
    if (fluidPath.empty()) return false;
    std::string abs = asset::AssetManager::ResolveAssetPath(fluidPath);
    if (abs.empty() || !util::FileSystem::Exists(abs)) abs = fluidPath;
    return m_impl->KeyBusy(PathKey(abs));
}

bool FluidBakeService::IsAnyBusy() const
{
    return !m_impl->active.empty() || !m_impl->orphans.empty();
}

void FluidBakeService::Tick(EditorContext& ctx)
{
    Impl& impl = *m_impl;
    /// @note 門を閉じた本人 (Fluid Editor) が居なくなったら開け直す。閉じたまま残ると、次にプレビューを
    /// @note 見る誰かが «前のソルバーのまま» 動かなくなる。
    if (!impl.previewSwitchAsked && !impl.previewSwitchAllowed) {
        impl.previewSwitchAllowed = true;
        impl.baker.AllowPreviewSwitch(true);
    }
    impl.previewSwitchAsked = false;

    std::erase_if(impl.orphans, [](Impl::Orphan& orphan) {
        const auto ready = [](auto& future) {
            return !future.valid() || future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        };
        if (!ready(orphan.bake) || !ready(orphan.preview)) return false;
        RemoveFluidBakeStage(std::move(orphan.stageDirectory));
        return true;
    });
    if (!impl.reloadQueue.empty()) {
        asset::AssetManager::ReloadPath(impl.reloadQueue.front());
        impl.reloadQueue.pop_front();
    }

    /// @note Retire が active を縮めるので、id を控えてから 1 件ずつ引き直す。
    std::vector<std::uint32_t> ids;
    ids.reserve(impl.active.size());
    for (const auto& job : impl.active) ids.push_back(job->status.id);
    for (const std::uint32_t id : ids) {
        const auto it = std::find_if(impl.active.begin(), impl.active.end(),
                                     [id](const std::unique_ptr<Impl::Job>& job) { return job->status.id == id; });
        if (it == impl.active.end()) continue;
        Impl::Job& job = **it;
        if (job.volume) continue;
        if (job.status.state == FluidJobState::Queued) {
            impl.StartFlatJob(job);
            continue;
        }
        job.status.progress = job.progress->load(std::memory_order_relaxed);
        if (job.status.kind == FluidJobKind::Bake) {
            job.status.simulationSeconds = job.timing->simulationSeconds.load(std::memory_order_relaxed);
            job.status.renderSeconds = job.timing->renderSeconds.load(std::memory_order_relaxed);
            job.status.slowestSimulationFrameSeconds = job.timing->slowestSimulationFrameSeconds.load(std::memory_order_relaxed);
            job.status.slowestSimulationFrame = job.timing->slowestSimulationFrame.load(std::memory_order_relaxed);
            job.status.slowestRenderFrameSeconds = job.timing->slowestRenderFrameSeconds.load(std::memory_order_relaxed);
            job.status.slowestRenderFrame = job.timing->slowestRenderFrame.load(std::memory_order_relaxed);
            job.status.stage = job.timing->stage.load(std::memory_order_relaxed);
            job.status.elapsedSeconds = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - job.started).count();
            job.status.outputSeconds = job.status.stage == 2
                ? (std::max)(0.0f, job.status.elapsedSeconds - job.status.simulationSeconds - job.status.renderSeconds)
                : 0.0f;
            job.status.remainingSeconds = job.status.progress > 0.02f && job.status.progress < 0.95f
                ? job.status.elapsedSeconds * (1.0f - job.status.progress) / job.status.progress : -1.0f;
        }
        if (!job.effectLayers.empty())
            job.status.progress = (static_cast<float>(job.effectIndex) + job.status.progress)
                / static_cast<float>(job.effectLayers.size());
        if (job.flatBake.valid()
            && job.flatBake.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            impl.FinishFlatBake(job, ctx);
        } else if (job.flatPreview.valid()
                   && job.flatPreview.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const PreviewOutcome outcome = job.flatPreview.get();
            job.status.message = outcome.message;
            job.status.fingerprint = outcome.fingerprint;
            job.status.previewFrame = outcome.frame;
            job.status.seed = outcome.seed;
            job.status.solverUsed = "cpu";
            if (!outcome.success) {
                job.status.state = FluidJobState::Failed;
            } else {
                job.status.state = FluidJobState::Done;
                job.status.progress = 1.0f;
            }
            impl.Retire(job, {});
        }
    }

    if (ctx.renderer == nullptr || ctx.resources == nullptr) return;
    if (impl.volumeJob == nullptr && !impl.baker.IsBusy()) {
        for (const auto& job : impl.active) {
            if (!job->volume || job->status.state != FluidJobState::Queued) continue;
            impl.StartVolumeJob(*job, ctx);
            /// @note Begin に失敗したジョブは Retire 済みで、active のイテレーターは無効になっている。
            break;
        }
    }
    if (impl.volumeJob == nullptr) return;
    Impl::Job& job = *impl.volumeJob;
    if (job.status.kind == FluidJobKind::Bake) impl.TickVolumeBake(job, ctx);
    else                                       impl.TickVolumePreview(job, ctx);
}

void FluidBakeService::Shutdown(EditorContext& ctx)
{
    Impl& impl = *m_impl;
    std::vector<std::uint32_t> ids;
    for (const auto& job : impl.active) ids.push_back(job->status.id);
    for (const std::uint32_t id : ids) (void)Cancel(id);
    renderer::ResourceManager* resources = ctx.resources != nullptr ? ctx.resources : impl.bakerResources;
    if (resources != nullptr) impl.baker.Release(*resources);
    else                      impl.baker.Cancel();
    impl.bakerResources = nullptr;
    /// @note Encoding 中の Cancel は Tick で確定するため、終了時だけ worker の停止後に仮出力を掃除する。
    for (const auto& job : impl.active)
        if (!job->stageDirectory.empty()) RemoveFluidBakeStage(job->stageDirectory);
    /// @note 中断直後に残る 2D の WIC 書き込みはここで待つ (future の破棄が完了を待つ)。
    for (Impl::Orphan& orphan : impl.orphans) {
        if (orphan.bake.valid()) orphan.bake.wait();
        if (orphan.preview.valid()) orphan.preview.wait();
        RemoveFluidBakeStage(std::move(orphan.stageDirectory));
    }
    impl.orphans.clear();
}

const asset::FluidBakeResult* FluidBakeService::FindFlatResult(std::uint32_t id) const
{
    const Impl::Finished* record = m_impl->FindFinished(id);
    return record != nullptr && record->hasFlat ? &record->flat : nullptr;
}

const asset::VolumeFlipbookBakeResult* FluidBakeService::FindVolumeResult(std::uint32_t id) const
{
    const Impl::Finished* record = m_impl->FindFinished(id);
    return record != nullptr && record->hasVolume ? &record->volume : nullptr;
}

const asset::VolumeFlipbookBakeSettings* FluidBakeService::FindVolumeSettings(std::uint32_t id) const
{
    const Impl::Finished* record = m_impl->FindFinished(id);
    return record != nullptr && record->hasVolume ? &record->volumeSettings : nullptr;
}

bool FluidBakeService::TakeBakedVolumeFlipbook(std::uint32_t id, asset::BakedVolumeFlipbook& out)
{
    if (id == 0 || id != m_impl->bakerResultJob) return false;
    return m_impl->baker.TakeBakedFlipbook(out);
}

bool FluidBakeService::RecordVolumePreview(EditorContext& ctx, const asset::VolumeFlipbookBakeSettings& settings,
                                           float time, const asset::VolumePreviewOptions& options)
{
    return RecordVolumePreview(ctx, settings, time, options, nullptr, 0);
}

bool FluidBakeService::RecordVolumePreview(EditorContext& ctx, const asset::VolumeFlipbookBakeSettings& settings,
                                           float time, const asset::VolumePreviewOptions& options,
                                           const fluid::FluidRecipe* recipe, std::uint64_t recipeRevision)
{
    if (!IsVolumeBakerFree() || ctx.renderer == nullptr || ctx.resources == nullptr) return false;
    m_impl->bakerResources = ctx.resources;
    m_impl->baker.RecordPreview(*ctx.renderer, *ctx.resources, settings, time, options, recipe, recipeRevision);
    return true;
}

bool FluidBakeService::IsVolumeBakerFree() const
{
    return m_impl->volumeJob == nullptr && !m_impl->baker.IsBusy();
}

void FluidBakeService::AllowVolumePreviewSwitch(bool allow)
{
    Impl& impl = *m_impl;
    impl.previewSwitchAsked = true;
    /// @note 焼きが握っている間の切り替えはその焼きのもの。プレビューの都合で止めない。
    if (!IsVolumeBakerFree()) return;
    impl.previewSwitchAllowed = allow;
    impl.baker.AllowPreviewSwitch(allow);
}

bool FluidBakeService::VolumePreviewSwitchPending() const
{
    return IsVolumeBakerFree() && m_impl->baker.HasPendingPreviewSwitch();
}

bool FluidBakeService::VolumePreviewStale() const
{
    return IsVolumeBakerFree() && m_impl->baker.IsPreviewStale();
}

const std::string& FluidBakeService::VolumePreviewNote() const
{
    return m_impl->baker.PreviewNote();
}

bool FluidBakeService::VolumePreviewNoteIsFailure() const
{
    return m_impl->baker.PreviewNoteIsFailure();
}

const asset::VolumeFlipbookBaker& FluidBakeService::VolumeBaker() const
{
    return m_impl->baker;
}

} // namespace fbzz::editor
