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
#include <Engine/Asset/FluidBaker.hpp>
#include <Engine/Asset/FluidRecipe.hpp>
#include <Engine/Asset/FluidSolver.hpp>
#include <Engine/Asset/FluidStepping.hpp>
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
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <future>
#include <numeric>
#include <system_error>
#include <utility>

namespace fbzz::editor {
namespace {

constexpr std::size_t kFinishedJobsKept = 32;
// 3D プレビューは CPU で解く液体だと 1 コマ目まで数十秒かかる。これを超えたら諦める。
constexpr float kVolumePreviewTimeoutSeconds = 180.0f;
// 解けたのに読み戻せない (RT が作れていない) まま回り続けないよう、連続失敗の上限を置く。
constexpr int kVolumePreviewReadbackRetries = 4;

// WIC は呼び出しスレッドで COM が初期化されている必要がある。自分が初期化した分だけ戻す
// (メインスレッドの STA では RPC_E_CHANGED_MODE で素通りする)。
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

struct PreviewOutcome {
    bool success = false;
    std::string message;
    std::string fingerprint;
    int frame = 0;
    std::uint32_t seed = 0;
};

asset::FluidShading EffectiveShading(const asset::FluidRecipe& recipe)
{
    if (recipe.kind == asset::FluidKind::Liquid) return asset::FluidShading::Liquid;
    return recipe.render.shading == asset::FluidShading::Liquid ? asset::FluidShading::Smoke : recipe.render.shading;
}

// Inspector のプレビューと同じ式で市松の上へ合成した、不透明な 8bit の絵にする。
// WHY 透過のまま出さないか: 読む側 (AI・画像ビューアー) ごとに下地が違い、同じ PNG が別の絵に見える。
std::vector<std::uint8_t> CompositeFrame(const asset::FluidFrameImage& frame, asset::FluidShading shading)
{
    const bool premultiplied = asset::FluidShadingIsPremultiplied(shading);
    const bool additive = shading == asset::FluidShading::Glow;
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

// 合成済みのタイルをシートの (originX, originY) へ貼る。
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

// 2D のプレビュー (別スレッド)。**焼きと同じ入口** (RenderFluidBakeFrames) でコマを解くので、
// 見えている絵は焼いたアトラスのそのコマと 1 画素まで同じ。
PreviewOutcome RunFlatPreview(const asset::FluidRecipe& source, const FluidPreviewRequest& request,
                              const std::filesystem::path& pngPath, std::atomic<float>& progress,
                              const std::atomic<bool>& cancel)
{
    asset::FluidRecipe recipe = source;
    if (request.seed != 0) recipe.seed = request.seed;
    const asset::FluidStepPlan plan = asset::MakeFluidStepPlan(recipe);
    const int frame = request.frame >= 0 ? std::clamp(request.frame, 0, plan.frameCount - 1)
                                         : plan.FrameOfTime(request.time);
    const int variants = std::clamp(request.variants, 1, 16);
    const bool seedSheet = request.contactSheet && variants > 1;
    const int sheetSize = std::clamp(request.size, 32, 2048);
    const asset::FluidShading shading = EffectiveShading(recipe);

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
        // seed だけを振った試し。«ばらつきが欲しい» は乱数ではなく seed の並べ方で満たす —
        // 気に入った 1 枚の seed をそのまま .fluid へ書けば、同じ絵が何度でも焼ける。
        for (int i = 0; i < variants; ++i) {
            if (cancel.load(std::memory_order_relaxed)) return { false, "キャンセルしました" };
            asset::FluidRecipe variant = recipe;
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
        /// 同じ .fluid の重複を弾く鍵 (Analytic の 3D は空)。
        std::string key;
        std::string fluidAbs;
        std::string projectRoot;
        asset::FluidRecipe recipe;
        bool volume = false;
        FluidBakeRequest bakeRequest;
        std::vector<FluidEffectLayer> effectLayers;
        std::vector<std::string> effectFiles;
        std::vector<std::string> effectMaterials;
        std::string effectVfxPath;
        std::size_t effectIndex = 0;
        FluidPreviewRequest previewRequest;
        asset::VolumeFlipbookBakeSettings volumeSettings;

        // WHY shared_ptr か: キャンセルした 2D ジョブのスレッドは止められないので、
        //     Job を捨てた後もスレッドが書き続けられるよう寿命をスレッド側にも持たせる。
        std::shared_ptr<std::atomic<float>> progress = std::make_shared<std::atomic<float>>(0.0f);
        std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
        std::future<asset::FluidBakeResult> flatBake;
        std::future<PreviewOutcome> flatPreview;

        bool previewRecorded = false;
        int readbackMisses = 0;
        std::chrono::steady_clock::time_point started;
    };

    struct Finished {
        FluidJobStatus status;
        bool hasFlat = false;
        asset::FluidBakeResult flat;
        bool hasVolume = false;
        asset::VolumeFlipbookBakeResult volume;
        asset::VolumeFlipbookBakeSettings volumeSettings;
    };

    /// キャンセル後もまだ書いている 2D のスレッド。std::future は捨てると完了まで待つので、
    /// 終わるまでここで持ち、同じ .fluid の次の焼きとファイルを取り合わないよう IsBusy にも数える。
    struct Orphan {
        std::string key;
        std::future<asset::FluidBakeResult> bake;
        std::future<PreviewOutcome> preview;
    };

    std::vector<std::unique_ptr<Job>> active;
    std::deque<Finished> finished;
    std::vector<Orphan> orphans;
    asset::VolumeFlipbookBaker baker;
    renderer::ResourceManager* bakerResources = nullptr;
    Job* volumeJob = nullptr;
    /// baker が今 Atlas を持っている 3D の焼きの id (TakeBakedFlipbook の持ち主)。
    std::uint32_t bakerResultJob = 0;
    std::uint32_t nextId = 1;
    /// 前のフレームに AllowVolumePreviewSwitch が呼ばれたか / そのとき渡した可否。
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
        Retire(job, {});
    }

    // 焼いた .fluid の隣の .mat と、頼まれていれば 1 層の .vfx を書く。失敗は message に足す (焼き自体は成功)。
    void WriteDerivedAssets(Job& job, const FluidMaterialSource& source, float lifetime, EditorContext& ctx)
    {
        const std::string sibling = SiblingMaterialPath(job.fluidAbs);
        if (job.bakeRequest.updateMaterial) {
            bool created = false;
            std::string error;
            if (WriteFluidParticleMaterial(sibling, source, created, error)) {
                job.status.materialPath = sibling;
                asset::AssetManager::ReloadPath(sibling);
                job.status.message += created ? "\n.mat を作りました: " : "\n.mat を更新しました: ";
                job.status.message += NormalizeAssetPath(sibling);
            } else {
                job.status.message += "\n" + error;
            }
        }
        if (!job.bakeRequest.vfxPath.empty()) {
            const std::string material = job.status.materialPath.empty() && util::FileSystem::Exists(sibling)
                ? sibling : job.status.materialPath;
            if (material.empty()) {
                job.status.message += "\n.vfx には .mat が要ります (updateMaterial を有効にしてください)";
            } else {
                const std::filesystem::path vfx = util::FileSystem::PathFromUtf8(job.bakeRequest.vfxPath);
                std::error_code directoryError;
                std::filesystem::create_directories(vfx.parent_path(), directoryError);
                const std::string rootName = job.bakeRequest.vfxRootName.empty() ? StemOf(job.fluidAbs)
                                                                                 : job.bakeRequest.vfxRootName;
                if (WriteSingleEmitterVfx(vfx, rootName, NormalizeAssetPath(material), lifetime)) {
                    job.status.vfxPath = util::FileSystem::PathToUtf8(vfx);
                    job.status.message += "\n.vfx を書きました: " + NormalizeAssetPath(job.status.vfxPath);
                } else {
                    job.status.message += "\n.vfx を書き出せません: " + job.bakeRequest.vfxPath;
                }
            }
        }
        asset::AssetManager::FlushFailed();
        ctx.requestAssetBrowserRefresh = true;
    }

    void FinishFlatBake(Job& job, EditorContext& ctx)
    {
        const asset::FluidBakeResult result = job.flatBake.get();
        Finished record;
        record.hasFlat = true;
        record.flat = result;
        job.status.progress = 1.0f;
        job.status.message = result.message;
        job.status.fingerprint = result.fingerprint;
        job.status.solverUsed = result.solverUsed;
        job.status.fallbackReason = result.fallbackReason;
        job.status.seed = job.recipe.seed;
        if (!result.success) {
            job.status.state = FluidJobState::Failed;
            Retire(job, std::move(record));
            return;
        }
        // 同じパスへ上書きしたので、読み込み済みのテクスチャ / 場を差し替える。
        for (const std::string* path : { &result.albedoPath, &result.motionVectorPath, &result.vectorFieldPath }) {
            if (path->empty()) continue;
            job.status.outputs.push_back(*path);
            (void)asset::AssetDatabase::GuidFromPath(*path);
            asset::AssetManager::ReloadPath(*path);
        }
        InvalidateFlipbookAtlasPreview();
        WriteDerivedAssets(job, FluidMaterialSource::FromFlat(result),
                           (std::max)(job.recipe.output.duration, 0.1f), ctx);
        if (!job.effectLayers.empty()) {
            if (job.status.materialPath.empty()) {
                Fail(job, "テンプレートの素材を保存できません: " + job.status.message);
                return;
            }
            job.effectMaterials.push_back(job.status.materialPath);
            job.status.outputs.push_back(job.status.materialPath);
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
        const asset::VolumeFlipbookBakeResult& result = baker.Result();
        Finished record;
        record.hasVolume = true;
        record.volume = result;
        record.volumeSettings = job.volumeSettings;
        job.status.message = result.message;
        job.status.fingerprint = result.fingerprint;
        job.status.solverUsed = result.solverUsed;
        job.status.fallbackReason = result.fallbackReason;
        job.status.seed = job.recipe.seed;
        if (baker.State() == asset::VolumeFlipbookBakeState::Idle) {
            job.status.state = FluidJobState::Cancelled;
            Retire(job, std::move(record));
            return;
        }
        if (!result.success) {
            job.status.state = FluidJobState::Failed;
            Retire(job, std::move(record));
            return;
        }
        bakerResultJob = job.status.id;
        job.status.progress = 1.0f;
        for (const std::string* path : { &result.colorPath, &result.motionPath, &result.sixWayPositivePath,
                                         &result.sixWayNegativePath }) {
            if (path->empty()) continue;
            job.status.outputs.push_back(*path);
            asset::AssetManager::ReloadPath(*path);
            // PNG は «直せる原本» として DDS の隣に書かれている。
            std::filesystem::path png = util::FileSystem::PathFromUtf8(*path);
            png.replace_extension(".png");
            if (util::FileSystem::Exists(png)) job.status.outputs.push_back(util::FileSystem::PathToUtf8(png));
        }
        if (!job.fluidAbs.empty()) {
            const float lifetime = (std::max)(static_cast<float>(result.frameCount) * job.volumeSettings.source.frameDt,
                                              0.1f);
            WriteDerivedAssets(job, FluidMaterialSource::FromVolume(result, job.volumeSettings), lifetime, ctx);
        } else {
            asset::AssetManager::FlushFailed();
            ctx.requestAssetBrowserRefresh = true;
        }
        job.status.state = FluidJobState::Done;
        Retire(job, std::move(record));
    }

    void StartVolumeJob(Job& job, EditorContext& ctx)
    {
        job.started = std::chrono::steady_clock::now();
        bakerResources = ctx.resources;
        if (job.status.kind == FluidJobKind::Preview) {
            job.status.state = FluidJobState::Running;
            job.status.message = "プレビューを解いています";
            volumeJob = &job;
            return;
        }
        std::string error;
        if (!baker.Begin(job.volumeSettings, *ctx.resources, error)) {
            Fail(job, error);
            return;
        }
        // Begin で baker の前の Atlas は捨てられる。
        bakerResultJob = 0;
        job.status.state = FluidJobState::Running;
        job.status.message = "焼いています";
        volumeJob = &job;
    }

    void TickVolumeBake(Job& job, EditorContext& ctx)
    {
        baker.Tick(*ctx.renderer, *ctx.resources);
        const int total = (std::max)(baker.TotalFrames(), 1);
        job.status.progress = std::clamp(static_cast<float>(baker.CompletedFrames()) / static_cast<float>(total),
                                         0.0f, 1.0f);
        if (baker.State() == asset::VolumeFlipbookBakeState::Encoding) {
            job.status.state = FluidJobState::Encoding;
            job.status.message = "書き出しています (BC7 圧縮)";
        }
        if (!baker.IsBusy()) FinishVolumeBake(job, ctx);
    }

    // RecordPreview で描いた次のフレームで読み戻す (読み戻しの WHY は VolumeFlipbookBaker.hpp)。
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
        job.status.state = FluidJobState::Running;
        const asset::FluidRecipe recipe = job.recipe;
        std::shared_ptr<std::atomic<float>> progress = job.progress;
        if (job.status.kind == FluidJobKind::Bake) {
            const std::filesystem::path base = util::FileSystem::PathFromUtf8(job.fluidAbs).replace_extension();
            const std::string basePath = util::FileSystem::PathToUtf8(base);
            job.status.message = job.effectLayers.empty() ? "焼いています"
                : "素材 " + std::to_string(job.effectIndex + 1) + "/" + std::to_string(job.effectLayers.size())
                  + ": " + job.effectLayers[job.effectIndex].name;
            job.flatBake = std::async(std::launch::async, [recipe, basePath, progress]() {
                return asset::BakeFluid(recipe, basePath, progress.get());
            });
            return;
        }
        const FluidPreviewRequest request = job.previewRequest;
        const std::filesystem::path png = util::FileSystem::PathFromUtf8(job.status.previewPngPath);
        std::shared_ptr<std::atomic<bool>> cancel = job.cancel;
        job.status.message = "プレビューを解いています";
        job.flatPreview = std::async(std::launch::async, [recipe, request, png, progress, cancel]() {
            return RunFlatPreview(recipe, request, png, *progress, *cancel);
        });
    }

    // 読み込み前の検証。成功したら outAbs / outRecipe を埋める。
    [[nodiscard]] bool ResolveFluid(const EditorContext& ctx, const std::string& path, std::string& outAbs,
                                    asset::FluidRecipe& outRecipe, FluidJobError& outError) const
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
    asset::FluidRecipe recipe;
    if (!m_impl->ResolveFluid(ctx, request.fluidPath, abs, recipe, outError)) return 0;
    const bool volume = recipe.bake.mode == asset::FluidBakeMode::Volume3D;
    if (volume && (ctx.renderer == nullptr || ctx.resources == nullptr)) {
        outError = { "NO_RENDERER", "3D の焼きにはレンダラーが要ります" };
        return 0;
    }
    // 3D は Baker がパスから .fluid を読み直すので、メモリ上の seed 差し替えは効かない。
    // 黙って «指定と違う seed» で焼くより、できないと言う。
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
    job->volume = volume;
    job->bakeRequest = request;
    job->status.fluidPath = abs;
    job->status.seed = job->recipe.seed;
    if (volume) job->volumeSettings = asset::MakeVolumeBakeSettings(job->recipe, abs);
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
    asset::FluidRecipe recipe;
    if (!m_impl->ResolveFluid(ctx, request.fluidPath, abs, recipe, outError)) return 0;
    const bool volume = recipe.bake.mode == asset::FluidBakeMode::Volume3D;
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
    // 見るコマはここで決めてしまう。2D も 3D も «焼きの第 n コマ» を指す 1 つの番号で話す。
    const asset::FluidStepPlan plan = asset::MakeFluidStepPlan(job->recipe);
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
    job->bakeRequest.updateMaterial = false;
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
        if (impl.volumeJob == &job && job.status.kind == FluidJobKind::Bake) impl.baker.Cancel();
        if (job.flatBake.valid() || job.flatPreview.valid()) {
            // BakeFluid は途中で止められない。書き終わるまで IsBusy に残す。
            impl.orphans.push_back({ job.effectLayers.empty() ? job.key : PathKey(job.fluidAbs),
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
    // 門を閉じた本人 (Fluid Editor) が居なくなったら開け直す。閉じたまま残ると、次にプレビューを
    // 見る誰かが «前のソルバーのまま» 動かなくなる。
    if (!impl.previewSwitchAsked && !impl.previewSwitchAllowed) {
        impl.previewSwitchAllowed = true;
        impl.baker.AllowPreviewSwitch(true);
    }
    impl.previewSwitchAsked = false;

    std::erase_if(impl.orphans, [](Impl::Orphan& orphan) {
        const auto ready = [](auto& future) {
            return !future.valid() || future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        };
        return ready(orphan.bake) && ready(orphan.preview);
    });

    // Retire が active を縮めるので、id を控えてから 1 件ずつ引き直す。
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
            // Begin に失敗したジョブは Retire 済みで、active のイテレーターは無効になっている。
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
    // 止められない 2D のスレッドはここで待つ (future の破棄が完了を待つ)。
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
                                           const asset::FluidRecipe* recipe, std::uint64_t recipeRevision)
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
    // 焼きが握っている間の切り替えはその焼きのもの。プレビューの都合で止めない。
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
